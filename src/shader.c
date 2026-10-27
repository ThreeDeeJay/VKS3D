/*
 * shader.c — SPIR-V stereo injection, deferred to pipeline creation
 *
 * Path A — pipeline has existing TCS+TES: patch TES with gl_ViewIndex.
 *
 * Path B — VS-based pipeline (no existing tessellation): patch VS directly
 *           with gl_ViewIndex. Works on any driver that properly implements
 *           VK_KHR_multiview (all current NVIDIA, AMD, Intel drivers).
 *           This replaces the TCS+TES injection approach which was a
 *           426.06-specific workaround and causes interface mismatch crashes
 *           on newer drivers due to strict PerVertex block validation.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include "stereo_icd.h"
#include "tes_inject.h"
#include "spirv/unified1/spirv.h"

#define SpvExecVertex           0
#define SpvExecTessEval         2
#define SpvExecGeometry         3
#define SpvStorageOutput        3
#define SpvStorageInput         1
#define SPIRV_MAGIC             0x07230203u

/* ── Dynamic SPIR-V word buffer ─────────────────────────────────────────── */
typedef struct {
    uint32_t *w;
    size_t n;
    size_t cap;
} SpvBuf;
static bool
sb_init(
    SpvBuf *b,
    size_t c)
{
    b->w = malloc(c * sizeof(uint32_t));
    b->n = 0;
    b->cap = c;
    return b->w != NULL;
}
static void
sb_free(
    SpvBuf *b)
{
    free(b->w);
    b->w = NULL;
    b->n = 0;
    b->cap = 0;
}
static bool
sb_push(
    SpvBuf *b,
    uint32_t v)
{
    if (b->n >= b->cap)
    {
        size_t new_cap = b->cap ? b->cap * 2 : 64;
        uint32_t *p = realloc(
            b->w,
            new_cap * sizeof(uint32_t));
        if (!p)
            return false;
        b->w = p;
        b->cap = new_cap;
    }
    b->w[b->n++] = v;
    return true;
}
static bool
sb_push_n(
    SpvBuf *b,
    const uint32_t *v,
    size_t c)
{
    for (size_t i = 0; i < c; i++)
    {
        if (!sb_push(b, v[i]))
            return false;
    }
    return true;
}
static inline uint32_t
op_(
    uint32_t op,
    uint32_t wc)
{
    return (wc << 16) | op;
}

/* ── Matrix provenance helpers ───────────────────────────────────────────── */

typedef struct
{
    const uint32_t *words;
    size_t          count;
    uint32_t bound;
    bool is_patchable;
    bool has_mv_cap;
    /* Diagnostics */
    bool has_emit_vertex;
    bool has_viewindex_builtin;
    /* Execution model */
    int exec_model;
    /* Builtins */
    uint32_t pos_var;
    uint32_t pos_member_idx;
    uint32_t pos_ptr_type;
    bool     pos_is_block;
    uint32_t pos_block_type[8];
    uint32_t pos_block_count;
    uint32_t view_var;
    /* Common types */
    uint32_t ft;
    uint32_t v4t;
    uint32_t it;
    uint32_t bt;
    uint32_t ptr_out_v4;
    uint32_t ptr_in_int;
    /* Entry point */
    uint32_t entry_function;
    size_t entry_function_word;
    size_t fn_word;
    /* Function writing Position */
    uint32_t position_function;
    /* Geometry */
    uint32_t emit_count;
    /* Shader analysis */
    uint32_t dot_count;
    bool has_matrix_ops;
    bool has_direct_position_write;
    /* Matrix provenance tracking */
    uint32_t value_capacity;
    uint8_t *value_from_matrix;
    uint8_t *is_matrix_type;
    uint8_t *is_matrix_ptr;
    /* Projection UBO discovery */
    uint32_t proj_struct_type;
    uint32_t proj_ptr_type;
    uint32_t proj_var;
    uint32_t proj_set;
    uint32_t proj_binding;
    uint32_t proj_member_mask;
    VkBool32 proj_found;
    /* projection load tracking */
    uint32_t proj_access_count;
    uint32_t proj_load_count;
    uint32_t proj_mtv_count;
    /* Projection provenance per SSA value */
    uint8_t *is_proj_value;
    uint8_t *is_view_value;
    /* ---------- NEW ---------- */
    /* Value was reconstructed from depth/screen-space rather than world-space. */
    uint8_t *is_screen_value;
    /* Number of reconstruction operations detected. */
    uint32_t screen_reconstruct_count;
} SpvMod;

static inline bool valid_id(const SpvMod *m, uint32_t id)
{
    return id < m->value_capacity;
}

static inline uint8_t matrix_value(const SpvMod *m, uint32_t id)
{
    return valid_id(m, id) ? m->value_from_matrix[id] : 0;
}

static inline void set_matrix_value(SpvMod *m, uint32_t id, uint8_t value)
{
    if (valid_id(m, id))
        m->value_from_matrix[id] = value;
}

static inline uint8_t matrix_ptr(const SpvMod *m, uint32_t id)
{
    return valid_id(m, id) ? m->is_matrix_ptr[id] : 0;
}

static inline void set_matrix_ptr(SpvMod *m, uint32_t id, uint8_t value)
{
    if (valid_id(m, id))
        m->is_matrix_ptr[id] = value;
}

static inline uint8_t matrix_type(const SpvMod *m, uint32_t id)
{
    return valid_id(m, id) ? m->is_matrix_type[id] : 0;
}

static inline void set_matrix_type(SpvMod *m, uint32_t id, uint8_t value)
{
    if (valid_id(m, id))
        m->is_matrix_type[id] = value;
}

static inline uint8_t proj_value(const SpvMod *m, uint32_t id)
{
    return valid_id(m, id) ? m->is_proj_value[id] : 0;
}

static inline void set_proj_value(SpvMod *m, uint32_t id, uint8_t value)
{
    if (valid_id(m, id))
        m->is_proj_value[id] = value;
}

static inline uint8_t view_value(const SpvMod *m, uint32_t id)
{
    return (id < m->value_capacity) ? m->is_view_value[id] : 0;
}

static inline void set_view_value(SpvMod *m, uint32_t id, uint8_t v)
{
    if (id < m->value_capacity)
        m->is_view_value[id] = v;
}

static inline uint8_t matrix_or2(const SpvMod *m,
                                 uint32_t a,
                                 uint32_t b)
{
    return matrix_value(m, a) | matrix_value(m, b);
}

static void free_spv_provenance(SpvMod *m)
{
    free(m->value_from_matrix);
    free(m->is_matrix_type);
    free(m->is_matrix_ptr);
    free(m->is_proj_value);
    free(m->is_view_value);
    m->value_from_matrix = NULL;
    m->is_matrix_type    = NULL;
    m->is_matrix_ptr     = NULL;
    m->is_proj_value     = NULL;
    m->is_view_value     = NULL;
    m->value_capacity = 0;
}

static uint64_t hash_spv(const uint32_t *data, size_t words);

static bool
spv_resolve_u32_constant(const SpvMod *m, uint32_t id, uint32_t *value)
{
    if (!m || !value || !m->words)
        return false;
    for (size_t i = 5; i < m->count; )
    {
        uint32_t op = m->words[i] & 0xffffu;
        uint32_t wc = m->words[i] >> 16;
        if (!wc || i + wc > m->count)
            break;
        if (op == SpvOpConstant && wc >= 4 && m->words[i + 2] == id)
        {
            *value = m->words[i + 3];
            return true;
        }
        i += wc;
    }
    return false;
}

static const char *
spv_op_name(uint32_t op);

static void do_scan(SpvMod *m, bool p2)
{
    const uint32_t *w=m->words;
    uint32_t current_function = 0;
    #define MAT(id)        matrix_value(m, (id))
    #define SETMAT(id,v)   set_matrix_value(m, (id), (v))
    #define PTR(id)        matrix_ptr(m, (id))
    #define SETPTR(id,v)   set_matrix_ptr(m, (id), (v))
    #define TYPE(id)       matrix_type(m, (id))
    #define SETTYPE(id,v)  set_matrix_type(m, (id), (v))
    #define PROJ(id)       proj_value(m, (id))
    #define SETPROJ(id,v)  set_proj_value(m, (id), (v))
    #define VIEW(id)       view_value(m, (id))
    #define SETVIEW(id,v)  set_view_value(m, (id), (v))
    for (size_t i=5;i<m->count;) {
        uint32_t op=w[i]&0xffff, wc=w[i]>>16;
        if (!wc||i+wc>m->count) break;
        if (!p2)
        {
        switch(op) {
            case SpvOpDot:
                m->dot_count++;
                break;
            case SpvOpAccessChain:
            case SpvOpInBoundsAccessChain:
            case SpvOpPtrAccessChain:
                if (wc >= 4 &&
                    w[i + 2] < m->value_capacity &&
                    w[i + 3] < m->value_capacity)
                {
                    SETPTR(w[i + 2], PTR(w[i + 3]));
                }
                if (wc >= 5 &&
                    w[i+3] == m->proj_var)
                {
                    uint32_t member_id = w[i + 4];
                    uint32_t member_value = member_id;
                    (void)spv_resolve_u32_constant(m, member_id, &member_value);
                    m->proj_access_count++;
                    m->proj_found = VK_TRUE;
                    /* projection provenance */
                    SETPROJ(w[i + 2], member_value + 1);
                    /* separate view provenance */
                    if (member_value == 2)
                        SETVIEW(w[i + 2], 1);
                    STEREO_LOG(
                        "PROJ_ACCESS result=%u base=%u index_id=%u member=%u",
                        w[i+2],
                        w[i+3],
                        member_id,
                        member_value);
                    STEREO_LOG(
                        "PROJ_ACCESS result=%u member=%u count=%u",
                        w[i+2],
                        member_value,
                        m->proj_access_count);
                }
                break;
            case SpvOpLoad:
                if (wc >= 4 &&
                    w[i + 2] < m->value_capacity &&
                    w[i + 3] < m->value_capacity)
                {
                    SETMAT(
                        w[i + 2],
                        MAT(w[i + 3]) || PTR(w[i + 3]));
                }
                if (wc >= 4)
                {
                    if (PROJ(w[i + 3]))
                    {
                        SETPROJ(w[i + 2], PROJ(w[i + 3]));
                        m->proj_load_count++;
                        STEREO_LOG(
                            "PROJ_LOAD id=%u src=%u count=%u proj=%u",
                            w[i+2],
                            w[i+3],
                            m->proj_load_count,
                            PROJ(w[i + 2]));
                    }
                    if (VIEW(w[i + 3]))
                    {
                        SETVIEW(w[i + 2], VIEW(w[i + 3]));
                    }
                }
                break;
            case SpvOpCompositeExtract:
                if (wc >= 5 &&
                    w[i + 2] < m->value_capacity &&
                    w[i + 3] < m->value_capacity)
                {
                    SETMAT(w[i + 2], MAT(w[i + 3]));
                    if (PROJ(w[i + 3]))
                        SETPROJ(w[i + 2], PROJ(w[i + 3]));
                    if (VIEW(w[i + 3]))
                        SETVIEW(w[i + 2], VIEW(w[i + 3]));
                }
                break;
            case SpvOpVectorShuffle:
                if (wc >= 6 &&
                    w[i + 2] < m->value_capacity &&
                    w[i + 3] < m->value_capacity)
                {
                    SETMAT(w[i + 2], MAT(w[i + 3]));
                    if (PROJ(w[i + 3]))
                        SETPROJ(w[i + 2], PROJ(w[i + 3]));
                    if (VIEW(w[i + 3]))
                        SETVIEW(w[i + 2], VIEW(w[i + 3]));
                }
                break;
            case SpvOpCompositeConstruct:
                if (wc >= 5 &&
                    w[i + 2] < m->value_capacity)
                {
                    uint8_t matrix = 0;
                    uint8_t proj = 0;
                    uint8_t view = 0;
                    for (uint32_t k = 3; k < wc; ++k)
                    {
                        uint32_t id = w[i + k];
                        if (id >= m->value_capacity)
                            continue;
                        matrix |= MAT(id);
                        if (!proj && PROJ(id))
                            proj = PROJ(id);
                        if (!view && VIEW(id))
                            view = VIEW(id);
                    }
                    SETMAT(w[i + 2], matrix);
                    if (proj)
                        SETPROJ(w[i + 2], proj);
                    if (view)
                        SETVIEW(w[i + 2], view);
                }
                break;
            case SpvOpCapability:
                if(wc>=2&&w[i+1]==SpvCapabilityMultiView) m->has_mv_cap=true;
                break;
            case SpvOpEntryPoint:
                if(wc>=3){
                    uint32_t e=w[i+1];
                    if(e==SpvExecVertex||e==SpvExecTessEval||e==SpvExecGeometry)
                    {
                        m->is_patchable=true;
                        m->exec_model=(int)e;
                        m->entry_function = w[i+2];
                    }}
                break;
            case SpvOpTypeFloat:
                if(wc==3&&w[i+2]==32) m->ft=w[i+1];
                break;
            case SpvOpTypeVector:
                if(wc==4&&w[i+2]==m->ft&&w[i+3]==4) m->v4t=w[i+1];
                break;
            case SpvOpTypeInt:
                if(wc==4&&w[i+2]==32) m->it=w[i+1];
                break;
            case SpvOpTypeMatrix:
                if (wc >= 4)
                {
                    if (w[i + 1] < m->value_capacity)
                        SETTYPE(w[i + 1], 1);
                }
                break;
            case SpvOpTypeStruct:
                if (wc >= 3)
                {
                    uint8_t matrix = 0;
                    for (uint32_t k = 2; k < wc; ++k)
                    {
                        if (w[i + k] < m->value_capacity &&
                            TYPE(w[i + k]))
                        {
                            matrix = 1;
                            break;
                        }
                    }
                    if (w[i + 1] < m->value_capacity)
                        SETTYPE(w[i + 1], matrix);
                    if (matrix)
                    {
                        STEREO_LOG(
                            "PROJ_STRUCT type=%u (previous=%u)",
                            w[i+1],
                            m->proj_struct_type);
                        m->proj_struct_type = w[i+1];
                    }
                }
                break;
            case SpvOpTypeArray:
                if (wc >= 4)
                {
                    STEREO_LOG(
                        "FS_TYPE_ARRAY id=%u elem=%u len=%u",
                        w[i + 1],
                        w[i + 2],
                        w[i + 3]);
                }
                break;
            case SpvOpTypeRuntimeArray:
                break;
            case SpvOpTranspose:
                if (wc >= 4 &&
                    w[i + 2] < m->value_capacity &&
                    w[i + 3] < m->value_capacity)
                {
                    SETMAT(w[i + 2], MAT(w[i + 3]));
                    if (PROJ(w[i + 3]))
                        SETPROJ(w[i + 2], PROJ(w[i + 3]));
                    if (VIEW(w[i + 3]))
                        SETVIEW(w[i + 2], VIEW(w[i + 3]));
                }
                break;
            case SpvOpMatrixTimesVector:
            case SpvOpMatrixTimesMatrix:
                m->has_matrix_ops = true;
                /* fall through */
            case SpvOpVectorTimesScalar:
            case SpvOpVectorTimesMatrix:
            case SpvOpMatrixTimesScalar:
                if (wc >= 5)
                {
                    if (w[i + 2] < m->value_capacity)
                    {
                        uint32_t a = w[i + 3];
                        uint32_t b = w[i + 4];
                        uint8_t proj_a = PROJ(a);
                        uint8_t proj_b = PROJ(b);
                        uint8_t view_a = VIEW(a);
                        uint8_t view_b = VIEW(b);
                        if (op == SpvOpMatrixTimesVector || op == SpvOpMatrixTimesMatrix)
                        {
                            STEREO_LOG(
                                "FS_MATRIX_MUL op=%s result=%u a=%u b=%u proj_a=%u proj_b=%u view_a=%u view_b=%u",
                                spv_op_name(op),
                                w[i + 2],
                                a,
                                b,
                                proj_a,
                                proj_b,
                                view_a,
                                view_b);
                        }
                        if ((proj_a || proj_b) &&
                            (op == SpvOpMatrixTimesVector ||
                             op == SpvOpMatrixTimesMatrix))
                        {
                            uint8_t proj = proj_a ? proj_a : proj_b;
                            m->proj_member_mask |= 1u << (proj - 1);
                            m->proj_mtv_count++;
                            STEREO_LOG(
                                "PROJ_MTV result=%u matrix=%u vector=%u member=%u mask=0x%X count=%u",
                                w[i + 2],
                                a,
                                b,
                                proj - 1,
                                m->proj_member_mask,
                                m->proj_mtv_count);
                        }
                        /* Do not automatically propagate projection provenance through
                         * MatrixTimesVector.
                         *
                         * Many fragment shaders (SSAO, SSR, depth reconstruction) multiply
                         * arbitrary vectors by the projection matrix without producing clip
                         * coordinates.
                         *
                         * Let later consumers decide whether this multiplication is actually
                         * part of a projection chain.
                         */
                        //if (proj_a || proj_b)
                        //    SETPROJ(w[i + 2], proj_a ? proj_a : proj_b);
                        if (view_a || view_b)
                            SETVIEW(w[i + 2], view_a ? view_a : view_b);
                        SETMAT(w[i + 2], 1);
                    }
                }
                break;
            case SpvOpCopyObject:
            case SpvOpBitcast:
                if (wc >= 4 &&
                    w[i + 2] < m->value_capacity &&
                    w[i + 3] < m->value_capacity)
                {
                    SETMAT(w[i + 2], MAT(w[i + 3]));
                    if (PROJ(w[i + 3]))
                        SETPROJ(w[i + 2], PROJ(w[i + 3]));
                    if (VIEW(w[i + 3]))
                        SETVIEW(w[i + 2], VIEW(w[i + 3]));
                }
                break;
            case SpvOpExtInst:
                if (wc >= 7 &&
                    w[i + 2] < m->value_capacity)
                {
                    uint8_t matrix = 0;
                    uint8_t proj = 0;
                    uint8_t view = 0;
                    for (uint32_t k = 5; k < wc; ++k)
                    {
                        uint32_t id = w[i + k];
                        if (id >= m->value_capacity)
                            continue;
                        matrix |= MAT(id);
                        if (!proj && PROJ(id))
                            proj = PROJ(id);
                        if (!view && VIEW(id))
                            view = VIEW(id);
                    }
                    SETMAT(w[i + 2], matrix);
                    if (proj)
                        SETPROJ(w[i + 2], proj);
                    if (view)
                        SETVIEW(w[i + 2], view);
                }
                break;
            case SpvOpFAdd:
            case SpvOpFSub:
            case SpvOpFMul:
            case SpvOpFDiv:
                if (wc >= 5 &&
                    w[i + 2] < m->value_capacity)
                {
                    SETMAT(
                        w[i + 2],
                        matrix_or2(m, w[i + 4], w[i + 5]));
                    if (PROJ(w[i + 4]))
                        SETPROJ(w[i + 2], PROJ(w[i + 4]));
                    else if (PROJ(w[i + 5]))
                        SETPROJ(w[i + 2], PROJ(w[i + 5]));
                    if (VIEW(w[i + 4]))
                        SETVIEW(w[i + 2], VIEW(w[i + 4]));
                    else if (VIEW(w[i + 5]))
                        SETVIEW(w[i + 2], VIEW(w[i + 5]));
                }
                break;
            case SpvOpSelect:
                if (wc >= 6 &&
                    w[i + 2] < m->value_capacity)
                {
                    SETMAT(
                        w[i + 2],
                        matrix_or2(m, w[i + 4], w[i + 5]));
                    if (PROJ(w[i + 4]))
                        SETPROJ(w[i + 2], PROJ(w[i + 4]));
                    else if (PROJ(w[i + 5]))
                        SETPROJ(w[i + 2], PROJ(w[i + 5]));
                    if (VIEW(w[i + 4]))
                        SETVIEW(w[i + 2], VIEW(w[i + 4]));
                    else if (VIEW(w[i + 5]))
                        SETVIEW(w[i + 2], VIEW(w[i + 5]));
                }
                break;
            case SpvOpFunctionCall:
                break;
            case SpvOpCompositeInsert:
                if (wc >= 6 &&
                    w[i + 2] < m->value_capacity)
                {
                    uint8_t matrix = 0;
                    uint8_t proj = 0;
                    uint8_t view = 0;
                    if (w[i + 3] < m->value_capacity)
                    {
                        matrix |= MAT(w[i + 3]);
                        if (PROJ(w[i + 3]))
                            proj = PROJ(w[i + 3]);
                        if (VIEW(w[i + 3]))
                            view = VIEW(w[i + 3]);
                    }
                    if (w[i + 4] < m->value_capacity)
                    {
                        matrix |= MAT(w[i + 4]);
                        if (!proj && PROJ(w[i + 4]))
                            proj = PROJ(w[i + 4]);
                        if (!view && VIEW(w[i + 4]))
                            view = VIEW(w[i + 4]);
                    }
                    SETMAT(w[i + 2], matrix);
                    if (proj)
                        SETPROJ(w[i + 2], proj);
                    if (view)
                        SETVIEW(w[i + 2], view);
                }
                break;
            case SpvOpTypePointer:
                if (wc >= 4)
                {
                if (TYPE(w[i + 3]))
                {
                SETPTR(w[i + 1], 1);
                }
                if (w[i+3] == m->proj_struct_type)
                {
                    STEREO_LOG(
                        "PROJ_PTR ptr=%u struct=%u",
                        w[i+1],
                        w[i+3]);
                    m->proj_ptr_type = w[i+1];
                }
                if (w[i + 2] == SpvStorageOutput &&
                m->v4t &&
                w[i + 3] == m->v4t)
                {
                m->ptr_out_v4 = w[i + 1];
                }
                if (w[i + 2] == SpvStorageInput &&
                m->it &&
                w[i + 3] == m->it)
                {
                m->ptr_in_int = w[i + 1];
                }
                }
                break;
            case SpvOpVariable:
                if (wc >= 4 &&
                    w[i + 1] < m->value_capacity &&
                    w[i + 2] < m->value_capacity &&
                    PTR(w[i + 1]))
                {
                    SETPTR(w[i + 2], 1);
                }
                if (w[i+1] == m->proj_ptr_type &&
                    w[i+3] == SpvStorageClassUniform)
                {
                    STEREO_LOG(
                        "PROJ_VAR_CANDIDATE var=%u ptr=%u previous=%u",
                        w[i+2],
                        w[i+1],
                        m->proj_var);
                    m->proj_var = w[i+2];
                }
                break;
            case SpvOpDecorate:
                if (wc >= 4)
                {
                    if (w[i+2] == SpvDecorationDescriptorSet &&
                        w[i+1] == m->proj_var)
                    {
                        m->proj_set = w[i+3];
                    }
                    if (w[i+2] == SpvDecorationBinding &&
                        w[i+1] == m->proj_var)
                    {
                        m->proj_binding = w[i+3];
                    }
                }
                if(wc>=4&&w[i+2]==SpvDecorationBuiltIn){
                    if(w[i+3]==SpvBuiltInPosition&&!m->pos_is_block)
                        m->pos_var=w[i+1];
                    if(w[i+3]==SpvBuiltInViewIndex) {
                        m->view_var = w[i+1];
                        m->has_viewindex_builtin = true;
                    }
                } break;
            case SpvOpMemberDecorate:
                if (wc >= 5 &&
                    w[i+3] == SpvDecorationBuiltIn &&
                    w[i+4] == SpvBuiltInPosition)
                {
                    if (m->pos_block_count < 8)
                        m->pos_block_type[m->pos_block_count++] = w[i+1];
                    m->pos_member_idx = w[i+2];
                    m->pos_is_block   = true;
                    m->pos_var        = 0;
                }
                break;
            case SpvOpFunction:
                if (!m->fn_word)
                    m->fn_word = i;
                if (wc >= 3)
                    current_function = w[i+2];
                break;
            case SpvOpFunctionEnd:
                current_function = 0;
                break;
            case SpvOpEmitVertex:
                m->emit_count++;
                m->has_emit_vertex = true;
                break;
            case SpvOpStore:
                if (wc >= 3 &&
                    w[i + 1] == m->pos_var)
                {
                    if (current_function &&
                        !m->position_function)
                    {
                        m->position_function = current_function;
                    }
                    uint32_t source = w[i + 2];
                    if (source >= m->value_capacity ||
                        !MAT(source))
                    {
                        m->has_direct_position_write = true;
                    }
                }
                break;
            }
        } else {
            if(op==SpvOpTypePointer && wc>=4 &&
               w[i+2]==SpvStorageOutput)
            {
                for(uint32_t k=0;k<m->pos_block_count;k++)
                {
                    if(w[i+3]==m->pos_block_type[k])
                    {
                        m->pos_ptr_type=w[i+1];
                        break;
                    }
                }
            }
            if(op==SpvOpVariable&&wc>=4&&w[i+3]==SpvStorageOutput)
            {
                if(m->pos_ptr_type &&
                   w[i+1]==m->pos_ptr_type)
                {
                    m->pos_var=w[i+2];
                }
            }
        }
        i+=wc;
    }
}

static void spv_scan(SpvMod *m)
{
    /* First pass: discover decorations/types. */
    do_scan(m, false);
    /*
     * Run again now that block Position info is known.
     * Some TES shaders declare OpTypePointer before
     * OpMemberDecorate(BuiltIn Position).
     */
    do_scan(m, false);
    if (m->pos_is_block)
        do_scan(m, true);
    STEREO_LOG(
        "SCAN hash=%016llx exec=%u matrix=%u proj=%u dot=%u direct=%u emit=%u pos=%u block=%u",
        (unsigned long long)hash_spv(m->words, m->count),
        m->exec_model,
        m->has_matrix_ops,
        m->proj_found,
        m->dot_count,
        m->has_direct_position_write,
        m->emit_count,
        m->pos_var,
        m->pos_is_block);
}

uint64_t hash_spv(const uint32_t *data, size_t words)
{
    uint64_t h = 1469598103934665603ULL; // FNV offset basis
    for (size_t i = 0; i < words; i++) {
        h ^= data[i];
        h *= 1099511628211ULL;
    }
    return h;
}

StereoPipelineInfo *
find_pipeline_info(
    StereoDevice *sd,
    VkPipeline pipeline)
{
    for (uint32_t i = 0; i < sd->pipeline_info_count; i++)
    {
        if (sd->pipeline_info[i].pipeline == pipeline)
            return &sd->pipeline_info[i];
    }

    return NULL;
}

VkPipeline
lookup_bound_pipeline(
    StereoDevice *sd,
    VkCommandBuffer cb)
{
    for (uint32_t i = 0; i < sd->cb_track_count; i++)
    {
        if (sd->cb_track[i].cb == cb)
            return sd->cb_track[i].pipeline;
    }

    return VK_NULL_HANDLE;
}

StereoDevice *
stereo_device_from_command_buffer(
    VkCommandBuffer cb)
{
    extern StereoDevice g_devices[];
    extern uint32_t g_device_count;

    for (uint32_t d = 0; d < g_device_count; d++)
    {
        StereoDevice *sd = &g_devices[d];

        for (uint32_t i = 0; i < sd->cb_track_count; i++)
        {
            if (sd->cb_track[i].cb == cb)
                return sd;
        }
    }

    return NULL;
}

void
remember_bound_pipeline(
    StereoDevice *sd,
    VkCommandBuffer cb,
    VkPipeline pipe)
{
    for (uint32_t i = 0; i < sd->cb_track_count; i++)
        {
        if (sd->cb_track[i].cb == cb)
        {
            sd->cb_track[i].pipeline = pipe;
            return;
            }
        }
        if (sd->cb_track_count >= MAX_CB_TRACK)
            return;
        CHECK_ARRAY_COUNT(sd->cb_track_count, MAX_CB_TRACK, "cb_track_count");
        uint32_t idx = sd->cb_track_count++;
        sd->cb_track[idx].cb = cb;
        sd->cb_track[idx].pipeline = pipe;
        sd->cb_track[idx].render_pass = VK_NULL_HANDLE;
        sd->cb_track[idx].framebuffer = VK_NULL_HANDLE;
        sd->cb_track[idx].subpass = 0;
}

void
remember_begin_renderpass(
    StereoDevice *sd,
    VkCommandBuffer cb,
    VkRenderPass rp,
    uint32_t subpass)
{
    for (uint32_t i = 0; i < sd->cb_track_count; i++)
    {
        if (sd->cb_track[i].cb == cb)
        {
            sd->cb_track[i].render_pass = rp;
            sd->cb_track[i].subpass = subpass;
            return;
        }
    }
    if (sd->cb_track_count >= MAX_CB_TRACK)
    return;
    CHECK_ARRAY_COUNT(sd->cb_track_count, MAX_CB_TRACK, "cb_track_count");
    uint32_t idx = sd->cb_track_count++;
    sd->cb_track[idx].cb = cb;
    sd->cb_track[idx].pipeline = VK_NULL_HANDLE;
    sd->cb_track[idx].render_pass = rp;
    sd->cb_track[idx].framebuffer = VK_NULL_HANDLE;
    sd->cb_track[idx].subpass = subpass;
}

VkRenderPass
lookup_bound_renderpass(
    StereoDevice *sd,
    VkCommandBuffer cb)
{
    for (uint32_t i = 0; i < sd->cb_track_count; i++)
    {
        if (sd->cb_track[i].cb == cb)
            return sd->cb_track[i].render_pass;
    }

    return VK_NULL_HANDLE;
}

VkFramebuffer
lookup_bound_framebuffer(
    StereoDevice *sd,
    VkCommandBuffer cb)
{
    for (uint32_t i = 0; i < sd->cb_track_count; i++)
    {
        if (sd->cb_track[i].cb == cb)
            return sd->cb_track[i].framebuffer;
    }

    return VK_NULL_HANDLE;
}

static StereoPipelineInfo *
add_pipeline_info(
    StereoDevice *sd)
{
    if (sd->pipeline_info_count >= sd->pipeline_info_capacity)
    {
        uint32_t new_cap =
            sd->pipeline_info_capacity ?
            sd->pipeline_info_capacity * 2 :
            128;
        StereoPipelineInfo *new_array =
            realloc(
            sd->pipeline_info,
            sizeof(*new_array) * new_cap);
        if (!new_array)
            return NULL;
        sd->pipeline_info = new_array;
        sd->pipeline_info_capacity = new_cap;
    }
    CHECK_ARRAY_COUNT(sd->pipeline_info_count, sd->pipeline_info_capacity, "pipeline_info_count");
    StereoPipelineInfo *info =
        &sd->pipeline_info[sd->pipeline_info_count++];
    memset(info, 0, sizeof(*info));
    return info;
}

/* ── Stereo offset injection body ────────────────────────────────────────── */
typedef struct {
    SpvMod *m;
    bool have_view;
    /* Provenance */
    bool has_projection_path;
    bool has_view_path;
    uint32_t uv4;
    uint32_t uint_;
    uint32_t bt;
    uint32_t cz;
    uint32_t cf0;
    uint32_t cl;
    uint32_t cr;
    uint32_t cc;
    uint32_t projection_mode;
    float lo_dbg;
    float ro_dbg;
    StereoDebugCtx *dbg;
} BodyCtx;

typedef struct StereoDebugCtx {
    uint32_t pipeline_index;
    VkRenderPass render_pass;
    bool is_multiview;
    uint32_t stage;
    uint32_t vertex_binding_count;
    uint32_t is_quad;
    VkBool32 has_proj_ubo;
    uint32_t proj_set;
    uint32_t proj_binding;
    uint32_t proj_member_mask;
    uint32_t proj_var;
    bool has_matrix_ops;
    bool direct_position_write;
} StereoDebugCtx;

static void emit_body(SpvBuf *out, const BodyCtx *c, uint32_t *nid)
{
    SpvMod *m = c->m;
    uint32_t ch = (*nid)++;
    uint32_t lp = (*nid)++;
    uint32_t pptr;
    if (m->pos_is_block)
    {
        uint32_t mid = (m->pos_member_idx == 0) ? c->cz : (*nid)++;
        if (m->pos_member_idx != 0)
        {
            uint32_t ci[] = {
                op_(SpvOpConstant, 4),
                m->it,
                mid,
                m->pos_member_idx
            };
            sb_push_n(out, ci, 4);
        }
        uint32_t a[] = {
            op_(SpvOpAccessChain, 5),
            c->uv4,
            ch,
            m->pos_var,
            mid
        };
        sb_push_n(out, a, 5);
        pptr = ch;
    }
    else
    {
        pptr = m->pos_var;
    }
    {
        uint32_t w[] = {
            op_(SpvOpLoad, 4),
            m->v4t,
            lp,
            pptr
        };
        sb_push_n(out, w, 4);
    }
    uint32_t lv = c->have_view ? (*nid)++ : 0;
    uint32_t isl = c->have_view ? (*nid)++ : 0;
    uint32_t sel = (*nid)++;
    uint32_t px = (*nid)++;
    uint32_t nx = (*nid)++;
    uint32_t np = (*nid)++;
    if (c->have_view && m->view_var && m->it && c->bt)
    {
        {
            uint32_t w[] = {
                op_(SpvOpLoad, 4),
                m->it,
                lv,
                m->view_var
            };
            sb_push_n(out, w, 4);
        }
        {
            uint32_t w[] = {
                op_(SpvOpIEqual, 5),
                c->bt,
                isl,
                lv,
                c->cz
            };
            sb_push_n(out, w, 5);
        }
        {
            uint32_t w[] = {
                op_(SpvOpSelect, 6),
                m->ft,
                sel,
                isl,
                c->cr,
                c->cl
            };
            sb_push_n(out, w, 6);
        }
    }
    else
    {
        sel = c->cl;
    }
    {
        uint32_t w[] = {
            op_(SpvOpCompositeExtract, 5),
            m->ft,
            px,
            lp,
            0u
        };
        sb_push_n(out, w, 5);
    }
    STEREO_LOG(
        "VS_PATCH "
        "mode=%d "
        "posVar=%u "
        "viewVar=%u "
        "block=%u "
        "member=%u "
        "haveView=%u "
        "convConst=%u "
        "leftConst=%u "
        "rightConst=%u",
        c->projection_mode,
        m->pos_var,
        m->view_var,
        m->pos_is_block,
        m->pos_member_idx,
        c->have_view,
        c->cc,
        c->cl,
        c->cr);
    STEREO_LOG(
        "VS_PATCH_IDS "
        "ch=%u "
        "lp=%u "
        "lv=%u "
        "isl=%u "
        "sel=%u "
        "px=%u "
        "nx=%u "
        "np=%u "
        "mode=%d "
        "pos_var=%u "
        "pptr=%u "
        "view_var=%u "
        "leftConst=%u "
        "rightConst=%u "
        "convConst=%u",
        ch,
        lp,
        lv,
        isl,
        sel,
        px,
        nx,
        np,
        c->projection_mode,
        m->pos_var,
        pptr,
        m->view_var,
        c->cl,
        c->cr,
        c->cc);
    if (c->projection_mode == STEREO_PROJECTION_PARALLEL)
    {
        uint32_t w[] = {
            op_(SpvOpFAdd, 5),
            m->ft,
            nx,
            px,
            sel
        };
        sb_push_n(out, w, 5);
    }
    else
    {
        uint32_t pw = (*nid)++;
        uint32_t convmag = (*nid)++;
        uint32_t negconv = (*nid)++;
        uint32_t convsel = (*nid)++;
        uint32_t tmp = (*nid)++;
        {
            uint32_t w[] = {
                op_(SpvOpCompositeExtract, 5),
                m->ft,
                pw,
                lp,
                3u
            };
            sb_push_n(out, w, 5);
        }
        {
            uint32_t w[] = {
                op_(SpvOpFMul, 5),
                m->ft,
                convmag,
                pw,
                c->cc
            };
            sb_push_n(out, w, 5);
        }
        {
            uint32_t w[] = {
                op_(SpvOpFSub, 5),
                m->ft,
                negconv,
                c->cf0,
                convmag
            };
            sb_push_n(out, w, 5);
        }
        {
            uint32_t w[] = {
                op_(SpvOpSelect, 6),
                m->ft,
                convsel,
                isl,
                convmag,
                negconv
            };
            sb_push_n(out, w, 6);
        }
        {
            uint32_t w[] = {
                op_(SpvOpFAdd, 5),
                m->ft,
                tmp,
                px,
                sel
            };
            sb_push_n(out, w, 5);
        }
        {
            uint32_t w[] = {
                op_(SpvOpFSub, 5),
                m->ft,
                nx,
                tmp,
                convsel
            };
            sb_push_n(out, w, 5);
        }
    }
    {
        uint32_t w[] = {
            op_(SpvOpCompositeInsert, 6),
            m->v4t,
            np,
            nx,
            lp,
            0u
        };
        sb_push_n(out, w, 6);
    }
    STEREO_LOG(
        "PROJ_WRITE pos_var=%u pptr=%u new_pos=%u x=%u view=%u",
        m->pos_var,
        pptr,
        np,
        nx,
        m->view_var);
    STEREO_LOG(
        "VIEWSPACE_PATCH "
        "mode=%d "
        "patching_outPos=%u "
        "projection_found=%u "
        "memberMask=0x%X",
        c->projection_mode,
        1,
        m->proj_found,
        c->dbg ? c->dbg->proj_member_mask : 0);
    {
        uint32_t w[] = {
            op_(SpvOpStore, 3),
            pptr,
            np
        };
        sb_push_n(out, w, 3);
    }
}

/* ── Public patcher ──────────────────────────────────────────────────────── */
bool spirv_patch_stereo_vertex(
    const StereoConfig *cfg,
    const uint32_t *in,
    size_t in_c,
    uint32_t **out,
    size_t *out_c,
    float lo,
    float ro,
    float conv,
    bool inj_vi,
    StereoDebugCtx *dbg)
{
    STEREO_LOG("CALLED spirv_patch_stereo_vertex");
    if (!in || in_c < 5 || in[0] != SPIRV_MAGIC)
        return false;
    int projection_mode =
        cfg ? cfg->projection : STEREO_PROJECTION_PARALLEL;
    SpvMod m = {0};
    m.words = in;
    m.count = in_c;
    m.bound = m.words[3];
    m.value_capacity = m.bound + 64;
    m.value_from_matrix =
        calloc(m.value_capacity, sizeof(uint8_t));
    m.is_matrix_type =
        calloc(m.value_capacity, sizeof(uint8_t));
    m.is_matrix_ptr =
        calloc(m.value_capacity, sizeof(uint8_t));
    m.is_proj_value =
        calloc(m.value_capacity, sizeof(uint8_t));
    m.is_view_value =
        calloc(m.value_capacity, sizeof(uint8_t));
    if (!m.value_from_matrix ||
        !m.is_matrix_type ||
        !m.is_matrix_ptr ||
        !m.is_proj_value ||
        !m.is_view_value)
    {
        free_spv_provenance(&m);
        return false;
    }
    /* Analyze shader structure before modification:
     * - matrix provenance
     * - gl_Position location
     * - ViewIndex availability
     * - entry point classification
     */
    spv_scan(&m);
    /*
     * Optional shader blacklist.
     * Used for debugging shaders that should remain untouched.
     */
    uint64_t spv_hash = hash_spv(m.words, m.count);

    //if (m.proj_found && m.proj_member_mask == 0x5)
    //{
    //    projection_mode = STEREO_PROJECTION_OFF_AXIS;
    //    STEREO_LOG(
    //        "PROJ_FIXUP forcing off-axis projection hash=%016llx mask=0x%X",
    //        (unsigned long long)spv_hash,
    //        m.proj_member_mask);
    //}

    /* Reject trivial passthrough vertex shaders.
     * World geometry always contains matrix math.
     * Fullscreen/UI shaders generally don't.
     */
    if (m.exec_model == SpvExecVertex)
    {
        if (!m.has_matrix_ops)
        {
            STEREO_LOG(
                "PATCH_SKIP no_matrix hash=%016llx exec=%u dots=%u direct=%u emit=%u pos=%u block=%u",
                (unsigned long long)spv_hash,
                m.exec_model,
                m.dot_count,
                m.has_direct_position_write,
                m.emit_count,
                m.pos_var,
                m.pos_is_block);
            free_spv_provenance(&m);
            return false;
        }
    }
    {
        static bool skip_list_init;
        static char skip_list[1024];
        if (!skip_list_init)
        {
            const char *env =
                stereo_getenv("VKS3D_SKIP_SHADER_PATCHES");
            if (env)
            {
                strncpy(skip_list, env, sizeof(skip_list) - 1);
                skip_list[sizeof(skip_list) - 1] = '\0';
            }
            skip_list_init = true;
        }
        if (skip_list[0])
        {
            char hashstr[17];
            snprintf(
                hashstr,
                sizeof(hashstr),
                "%016llx",
                (unsigned long long)spv_hash);
            if (strstr(skip_list, hashstr))
            {
                STEREO_LOG(
                    "SKIP_SHADER_PATCH hash=%s",
                    hashstr);
                free_spv_provenance(&m);
                return false;
            }
        }
    }
    /*
     * Reject known monoscopic screen-space shaders.
     *
     * These shaders usually write clip-space positions directly
     * and have no camera transform. Applying stereo offsets here
     * creates excessive negative parallax.
     */
    if (cfg && cfg->mono_ui)    {
        bool ui_candidate =
            dbg &&
            (dbg->is_quad ||
             dbg->vertex_binding_count == 0) &&
            !m.has_matrix_ops &&
            m.has_direct_position_write &&
            !m.has_emit_vertex &&
            m.exec_model == SpvExecVertex;
        if (ui_candidate)
        {
            STEREO_LOG(
                "SCREENSPACE_SKIP hash=%016llx exec=%u pos=%u block=%u matrix=%u direct=%u emit=%u",
                (unsigned long long)spv_hash,
                (unsigned)m.exec_model,
                m.pos_var,
                m.pos_is_block,
                m.has_matrix_ops,
                m.has_direct_position_write,
                m.has_emit_vertex);
            free_spv_provenance(&m);
            return false;
        }
    }
    if (dbg && !dbg->is_multiview)
    {
        STEREO_LOG(
            "PATCH_SKIP non-multiview render pass");
        free_spv_provenance(&m);
        return false;
    }
    STEREO_LOG(
        "PATCH_ANALYSIS hash=%016llx exec=%u patchable=%d pos=%u block=%d member=%u view=%u matrix=%d direct=%d dots=%u emit=%u mv=%d",
        (unsigned long long)spv_hash,
        m.exec_model,
        m.is_patchable,
        m.pos_var,
        m.pos_is_block,
        m.pos_member_idx,
        m.view_var,
        m.has_matrix_ops,
        m.has_direct_position_write,
        m.dot_count,
        m.emit_count,
        m.has_viewindex_builtin);
    STEREO_LOG(
        "PROJ_FINAL hash=%016llx exec=%u found=%u set=%u binding=%u mask=0x%X access=%u loads=%u mtv=%u",
        (unsigned long long)spv_hash,
        m.exec_model,
        m.proj_found,
        m.proj_set,
        m.proj_binding,
        m.proj_member_mask,
        m.proj_access_count,
        m.proj_load_count,
        m.proj_mtv_count);
    STEREO_LOG(
        "PROJ_DESCRIPTOR hash=%016llx struct=%u ptr=%u var=%u set=%u binding=%u",
        (unsigned long long)spv_hash,
        m.proj_struct_type,
        m.proj_ptr_type,
        m.proj_var,
        m.proj_set,
        m.proj_binding);
    if (m.proj_found)
    {
        STEREO_LOG(
            "PROJ_UBO hash=%016llx set=%u binding=%u mask=0x%X var=%u",
            (unsigned long long)spv_hash,
            m.proj_set,
            m.proj_binding,
            m.proj_member_mask,
            m.proj_var);
    }
    if (dbg)
    {
        dbg->has_matrix_ops = m.has_matrix_ops;
        dbg->direct_position_write = m.has_direct_position_write;
        dbg->has_proj_ubo = false;
        if (m.proj_found)
        {
            dbg->has_proj_ubo = true;
            dbg->proj_set = m.proj_set;
            dbg->proj_binding = m.proj_binding;
            dbg->proj_member_mask = m.proj_member_mask;
            dbg->proj_var = m.proj_var;
        }
        STEREO_LOG(
            "PROJ_DETECT hash=%016llx found=%u set=%u binding=%u mask=0x%X var=%u",
            (unsigned long long)spv_hash,
            m.proj_found,
            dbg->proj_set,
            dbg->proj_binding,
            dbg->proj_member_mask,
            dbg->proj_var);
        STEREO_LOG(
            "PROJ_TRACE access_count=%u load_count=%u mtv_count=%u mask=0x%X",
            m.proj_access_count,
            m.proj_load_count,
            m.proj_mtv_count,
            m.proj_member_mask);
    }
    if (m.exec_model == SpvExecVertex)
    {
        if (!m.pos_var)
        {
            STEREO_LOG(
                "PATCH_SKIP no gl_Position");
            free_spv_provenance(&m);
            return false;
        }
        if (m.pos_is_block && !m.has_matrix_ops)
        {
            STEREO_LOG(
                "PATCH_SKIP screen-space position block");
            free_spv_provenance(&m);
            return false;
        }
    }
    if (!m.is_patchable)
    {
        free_spv_provenance(&m);
        return false;
    }
    /* Allocate new SPIR-V IDs and prepare injected objects:
     * - output position pointer
     * - ViewIndex input
     * - stereo constants
     * - temporary types
     *
     * Future projection-matrix patching should extend this stage.
     */
    bool is_gs =
        (m.exec_model == SpvExecGeometry);
    uint32_t nid = m.bound;
    uint32_t id_ptr_v4 = nid++;
    uint32_t id_ptr_int = nid++;
    uint32_t id_new_it = 0;
    if (!m.it && inj_vi && !m.view_var)
    {
        id_new_it = nid++;
        m.it = id_new_it;
    }
    bool will_inj_vi =
        inj_vi &&
        !m.view_var &&
        m.it;
    uint32_t id_inj_view =
        will_inj_vi ? nid++ : 0;
    bool have_view =
        m.view_var ||
        will_inj_vi;
    uint32_t id_new_bt = 0;
    if (!m.bt && have_view && m.it)
        id_new_bt = nid++;
    uint32_t id_cz = nid++;
    uint32_t id_cf0 = nid++;
    uint32_t id_cl = nid++;
    uint32_t id_cr = nid++;
    uint32_t id_cc = nid++;
    uint32_t uv4 =
        m.ptr_out_v4 ?
        m.ptr_out_v4 :
        id_ptr_v4;
    uint32_t uint_ =
        m.ptr_in_int ?
        m.ptr_in_int :
        id_ptr_int;
    uint32_t bt =
        m.bt ?
        m.bt :
        id_new_bt;
    /* Additional SPIR-V declarations inserted before the entry function:
     * - new types
     * - constants
     * - ViewIndex variable
     */
    SpvBuf ann;
    SpvBuf te;
    if (!sb_init(&ann, 16) || !sb_init(&te, 96))
    {
        free_spv_provenance(&m);
        return false;
    }
    if (id_new_it)
    {
        uint32_t w[] =
        {
            op_(SpvOpTypeInt, 4),
            id_new_it,
            32,
            1
        };
        sb_push_n(&te, w, 4);
    }
    if (!m.ptr_out_v4)
    {
        uint32_t w[] =
        {
            op_(SpvOpTypePointer, 4),
            id_ptr_v4,
            SpvStorageOutput,
            m.v4t
        };
        sb_push_n(&te, w, 4);
    }
    if (m.it && !m.ptr_in_int)
    {
        uint32_t w[] =
        {
            op_(SpvOpTypePointer, 4),
            id_ptr_int,
            SpvStorageInput,
            m.it
        };
        sb_push_n(&te, w, 4);
        m.ptr_in_int = id_ptr_int;
        uint_ = id_ptr_int;
    }
    if (id_new_bt)
    {
        uint32_t w[] =
        {
            op_(SpvOpTypeBool, 2),
            id_new_bt
        };
        sb_push_n(&te, w, 2);
    }
    if (m.it)
    {
        uint32_t w[] =
        {
            op_(SpvOpConstant, 4),
            m.it,
            id_cz,
            0
        };
        sb_push_n(&te, w, 4);
    }
    {
        uint32_t w[4] =
        {
            op_(SpvOpConstant, 4),
            m.ft,
            id_cf0,
            0
        };
        float z = 0.0f;
        memcpy(&w[3], &z, sizeof(z));
        sb_push_n(&te, w, 4);
    }
    {
        uint32_t w[4] =
        {
            op_(SpvOpConstant, 4),
            m.ft,
            id_cl,
            0
        };
        memcpy(&w[3], &lo, sizeof(lo));
        sb_push_n(&te, w, 4);
    }
    {
        uint32_t w[4] =
        {
            op_(SpvOpConstant, 4),
            m.ft,
            id_cr,
            0
        };
        memcpy(&w[3], &ro, sizeof(ro));
        sb_push_n(&te, w, 4);
    }
    {
        uint32_t w[4] =
        {
            op_(SpvOpConstant, 4),
            m.ft,
            id_cc,
            0
        };
        memcpy(&w[3], &conv, sizeof(conv));
        sb_push_n(&te, w, 4);
    }
    if (will_inj_vi)
    {
        uint32_t d[] =
        {
            op_(SpvOpDecorate, 4),
            id_inj_view,
            SpvDecorationBuiltIn,
            SpvBuiltInViewIndex
        };
        sb_push_n(&ann, d, 4);
        uint32_t v[] =
        {
            op_(SpvOpVariable, 4),
            uint_,
            id_inj_view,
            SpvStorageInput
        };
        sb_push_n(&te, v, 4);
        m.view_var = id_inj_view;
    }
    BodyCtx bc =
    {
        .m                   = &m,
        .have_view           = have_view,
        .has_projection_path = false,
        .has_view_path       = false,
        .uv4                 = uv4,
        .uint_               = uint_,
        .bt                  = bt,
        .cz                  = id_cz,
        .cf0                 = id_cf0,
        .cl                  = id_cl,
        .cr                  = id_cr,
        .cc                  = id_cc,
        .projection_mode     = projection_mode,
        .lo_dbg              = lo,
        .ro_dbg              = ro,
        .dbg                 = dbg
    };
    /* Vertex/TessEval shaders:
     * inject after final position calculation.
     *
     * Geometry shaders:
     * inject before EmitVertex.
     */
    size_t ins_ann = 0;
    size_t ins_t = 0;
    size_t ins_b = 0;
    bool in_entry_function = false;
    for (size_t i = 5; i < in_c;)
    {
        uint32_t opx = in[i] & 0xffff;
        uint32_t wcx = in[i] >> 16;
        if (!wcx || i + wcx > in_c)
            break;
        if (!ins_ann &&
            (opx == SpvOpTypeVoid ||
             opx == SpvOpTypeBool ||
             opx == SpvOpTypeInt ||
             opx == SpvOpTypeFloat ||
             opx == SpvOpTypeVector ||
             opx == SpvOpTypeMatrix ||
             opx == SpvOpTypeImage ||
             opx == SpvOpTypeSampler ||
             opx == SpvOpTypeSampledImage ||
             opx == SpvOpTypeArray ||
             opx == SpvOpTypeRuntimeArray ||
             opx == SpvOpTypeStruct ||
             opx == SpvOpTypeOpaque ||
             opx == SpvOpTypePointer ||
             opx == SpvOpTypeFunction ||
             opx == SpvOpTypeForwardPointer ||
             opx == SpvOpConstantTrue ||
             opx == SpvOpConstantFalse ||
             opx == SpvOpConstant ||
             opx == SpvOpConstantComposite ||
             opx == SpvOpVariable))
        {
            ins_ann = i;
        }
        if (opx == SpvOpFunction)
        {
            in_entry_function =
                (wcx >= 4 &&
                 in[i + 2] ==
                 (m.position_function ?
                  m.position_function :
                  m.entry_function));
            if (in_entry_function)
                ins_t = i;
        }
        if (in_entry_function &&
            opx == SpvOpReturn)
        {
            ins_b = i;
        }
        if (in_entry_function &&
            opx == SpvOpFunctionEnd)
        {
            break;
        }
        i += wcx;
    }
    if (!ins_t)
    {
        sb_free(&te);
        free_spv_provenance(&m);
        return false;
    }
    if (!is_gs && !ins_b)
    {
        sb_free(&te);
        free_spv_provenance(&m);
        return false;
    }
    bool need_mv_cap =
        id_inj_view &&
        !m.has_mv_cap;
    bool mv_done = false;
    bool ann_done = false;
    bool te_done = false;
    bool body_done = false;
    /* Rebuild the SPIR-V module:
     * - add MultiView capability if required
     * - insert declarations
     * - extend entry point interface
     * - inject stereo body
     */
    SpvBuf ob;
    if (!sb_init(&ob, in_c + te.n + 64))
    {
        sb_free(&te);
        free_spv_provenance(&m);
        return false;
    }
    STEREO_LOG(
        "SPV_HEADER version=0x%08X generator=0x%08X bound=%u",
        in[1],
        in[2],
        in[3]);
    uint32_t spv_version = in[1];
    bool need_mv_ext =
        need_mv_cap &&
        ((spv_version >> 16) == 1) &&
        (((spv_version >> 8) & 0xff) == 0);
    STEREO_LOG(
        "SPV_MULTIVIEW version=0x%08X needExt=%u needCap=%u",
        spv_version,
        need_mv_ext,
        need_mv_cap);
    /* Header only. We'll inject after the last OpCapability. */
    sb_push_n(&ob, in, 5);
    bool ext_done = false;
    for (size_t i = 5; i < in_c;)
    {
        if (!mv_done && need_mv_cap)
        {
            uint32_t c[] =
            {
                op_(SpvOpCapability, 2),
                SpvCapabilityMultiView
            };
            sb_push_n(&ob, c, 2);
            mv_done = true;
        }
        if (!ann_done && i == ins_ann)
        {
            sb_push_n(&ob, ann.w, ann.n);
            ann_done = true;
        }
        if (!te_done && i == ins_t)
        {
            sb_push_n(&ob, te.w, te.n);
            te_done = true;
        }
        uint32_t opx = in[i] & 0xffff;
        uint32_t wcx = in[i] >> 16;
        if (!wcx || i + wcx > in_c)
            break;
        /* After the final OpCapability, emit OpExtension if required. */
        if (!ext_done &&
            need_mv_ext &&
            opx != SpvOpCapability)
        {
            uint32_t e[] =
            {
                op_(SpvOpExtension, 6),
                0x5F565053, /* SPV_ */
                0x5F52484B, /* KHR_ */
                0x746C756D, /* mult */
                0x65697669, /* ivie */
                0x00000077  /* w */
            };
            sb_push_n(&ob, e, 6);
            ext_done = true;
        }
        if (id_inj_view &&
            opx == SpvOpEntryPoint &&
            wcx >= 4 &&
            (in[i + 1] == SpvExecVertex ||
             in[i + 1] == SpvExecGeometry ||
             in[i + 1] == SpvExecTessEval))
        {
            bool target_entry =
                (wcx >= 3 &&
                 in[i + 2] == m.entry_function);
            if (target_entry)
            {
                sb_push(
                    &ob,
                    ((wcx + 1) << 16) |
                    SpvOpEntryPoint);
                sb_push_n(
                    &ob,
                    &in[i + 1],
                    wcx - 1);
                sb_push(
                    &ob,
                    id_inj_view);
            }
            else
            {
                sb_push_n(
                    &ob,
                    &in[i],
                    wcx);
            }
            i += wcx;
            continue;
        }
        STEREO_LOG(
            "VS_PATCH_SUMMARY pipeline=%u projFound=%u viewBuiltin=%u projMode=%u "
            "projVar=%u members=0x%X matrixOps=%u directPos=%u",
            dbg ? dbg->pipeline_index : 0,
            m.proj_found,
            m.has_viewindex_builtin,
            projection_mode,
            dbg ? dbg->proj_var : 0,
            dbg ? dbg->proj_member_mask : 0,
            dbg ? dbg->has_matrix_ops : 0,
            dbg ? dbg->direct_position_write : 0);
        if (is_gs &&
            opx == SpvOpEmitVertex)
        {
            emit_body(
                &ob,
                &bc,
                &nid);
        }
        if (!is_gs &&
            !body_done &&
            i == ins_b)
        {
            emit_body(
                &ob,
                &bc,
                &nid);
            body_done = true;
        }
        sb_push_n(
            &ob,
            &in[i],
            wcx);
        i += wcx;
    }
    /* Module contained only capabilities before declarations. */
    if (!ext_done && need_mv_ext)
    {
        uint32_t e[] =
        {
            op_(SpvOpExtension, 6),
            0x5F565053,
            0x5F52484B,
            0x746C756D,
            0x65697669,
            0x00000077
        };
        sb_push_n(&ob, e, 6);
    }
    if (!ann_done)
        sb_push_n(&ob, ann.w, ann.n);
    if (!te_done)
        sb_push_n(&ob, te.w, te.n);
    sb_free(&ann);
    sb_free(&te);
    ob.w[3] = nid;
    *out = ob.w;
    *out_c = ob.n;
    /*
     * Finalize patched SPIR-V module.
     * Provenance tables are no longer needed after reconstruction.
     */
    free_spv_provenance(&m);
    return true;
}

void spirv_patched_free(uint32_t *w) { free(w); }

/* ══════════════════════════════════════════════════════════════════════════
 * Fragment shader analysis state
 *
 * Tracks descriptor ownership, sampled-image propagation and function
 * parameter forwarding so the patcher can determine which image accesses
 * should become array-layered. Future projection-matrix analysis will
 * extend this structure rather than introducing another scanner.
 * ══════════════════════════════════════════════════════════════════════════
 */

#define FS_MAX_IMG         64
#define FS_MAX_SI          64
#define FS_MAX_LOADS      512
#define FS_MAX_PARAMS     256
#define FS_MAX_CALLS      256
#define FS_MAX_FUNCTIONS   64
#define FS_MAX_VARS       128

typedef struct
{
    uint32_t id;           /* OpLoad result id */
    uint32_t source_id;    /* Original source SSA id */
    uint32_t owner_var;    /* Descriptor variable owning this resource */
    uint32_t binding;      /* Cached binding after fixup */
    /* ---- Projection provenance ---- */
    bool     from_projection;
    bool     from_view;
} FsLoadInfo;

typedef struct
{
    uint32_t id;
    uint32_t type;
    uint32_t function_id;
    uint32_t index;
} FsParameterInfo;

typedef struct
{
    uint32_t id;
    uint32_t first_param;
} FsFunctionInfo;

typedef struct
{
    uint32_t function_id;
    uint32_t parameter_index; /* temporary parameter slot */
    uint32_t parameter_id;    /* resolved parameter id */
    uint32_t argument_var;    /* SSA id passed to the call */
} FsCallInfo;

typedef struct
{
    uint32_t id;
    uint32_t type;
    uint32_t storage;
    uint32_t set;
    uint32_t binding;
    uint32_t location;
    bool     is_projection_ubo;
} FsVariableInfo;

typedef struct
{
    uint32_t target;
    uint32_t binding;
    uint32_t set;
    uint32_t location;
} FsDecorationInfo;

typedef struct
{
    uint32_t id;
    uint32_t depth;
    uint32_t arrayed;
    bool     patchable;
    uint32_t sampled_type;   /* OpTypeSampledImage id wrapping this image type */
    uint32_t owner_var;
    uint32_t binding;
    uint32_t set;
} FsImageInfo;


typedef struct
{
    //Image type declarations
    FsImageInfo images[FS_MAX_IMG];
    uint32_t    n_img;
    //Sampled-image type declarations
    uint32_t si_ids[FS_MAX_SI];
    uint32_t n_si;
    //Resource ownership tables
    FsLoadInfo loads[FS_MAX_LOADS];
    uint32_t   n_load;
    FsParameterInfo params[FS_MAX_PARAMS];
    uint32_t        n_param;
    FsCallInfo calls[FS_MAX_CALLS];
    uint32_t   n_call;
    //Descriptor variables
    FsVariableInfo vars[FS_MAX_VARS];
    uint32_t       n_var;
    //Decorations
    FsDecorationInfo decorations[FS_MAX_VARS];
    uint32_t         n_dec;
    //Cached SPIR-V types
    uint32_t float_id;
    uint32_t int_id;
    uint32_t v3float_id;
    uint32_t ptr_int_in_id;
    uint32_t vi_var_id;
    bool     has_mv_cap;
    //Entry point information
    size_t ep_word;
    size_t fn_word;
    //Function table
    FsFunctionInfo functions[FS_MAX_FUNCTIONS];
    uint32_t       n_function;
    //Current function during prescan
    bool     in_function;
    uint32_t current_function_id;
    uint32_t current_param_index;
} FsScan;

static bool
fs_id_in(
    const uint32_t *arr,
    uint32_t n,
    uint32_t id)
{
    for (uint32_t i = 0; i < n; i++)
    {
        if (arr[i] == id)
            return true;
    }

    return false;
}

static int
fs_var_index(
    const FsScan *s,
    uint32_t id)
{
    if (!s)
        return -1;

    for (uint32_t i = 0; i < s->n_var; ++i)
    {
        if (s->vars[i].id == id)
            return (int)i;
    }

    return -1;
}

static void
fs_dump_descriptor_chain(
    const FsScan *s,
    const uint32_t *spv,
    size_t word_count,
    uint32_t descriptor_var)
{
    int vi = fs_var_index(s, descriptor_var);
    if (vi < 0)
        return;
    uint32_t type_id = s->vars[vi].type;
    STEREO_LOG(
        "FS_RESOURCE descriptor=%u varType=%u",
        descriptor_var,
        type_id);
    for (size_t i = 5; i < word_count; )
    {
        uint32_t wc = spv[i] >> 16;
        uint32_t op = spv[i] & 0xffff;
        if (!wc || i + wc > word_count)
            break;
        if ((op == SpvOpTypePointer ||
             op == SpvOpTypeSampledImage ||
             op == SpvOpTypeImage) &&
            spv[i + 1] == type_id)
        {
            STEREO_LOG(
                "FS_RESOURCE_TYPE id=%u op=%s",
                type_id,
                spv_op_name(op));
            if (op == SpvOpTypePointer && wc >= 4)
            {
                STEREO_LOG(
                    "FS_POINTER elementType=%u storage=%u",
                    spv[i + 3],
                    spv[i + 2]);
                type_id = spv[i + 3];
                i = 5;
                continue;
            }
            if (op == SpvOpTypeSampledImage && wc >= 3)
            {
                STEREO_LOG(
                    "FS_SAMPLED_IMAGE imageType=%u",
                    spv[i + 2]);
                type_id = spv[i + 2];
                i = 5;
                continue;
            }
            if (op == SpvOpTypeImage && wc >= 9)
            {
                STEREO_LOG(
                    "FS_IMAGE_TYPE sampledType=%u dim=%u depth=%u arrayed=%u ms=%u sampled=%u format=%u",
                    spv[i + 2],
                    spv[i + 3],
                    spv[i + 4],
                    spv[i + 5],
                    spv[i + 6],
                    spv[i + 7],
                    spv[i + 8]);
                break;
            }
        }
        i += wc;
    }
}

static int
fs_dec_index(
    const FsScan *s,
    uint32_t target)
{
    if (!s)
        return -1;

    for (uint32_t i = 0; i < s->n_dec; ++i)
    {
        if (s->decorations[i].target == target)
            return (int)i;
    }

    return -1;
}

/*
 * Returns true only for descriptors backed by upgraded stereo render
 * targets. Material textures, lookup tables and other resources remain
 * regular sampler2D objects.
 */
static bool
fs_binding_is_stereo_attachment(
const FsScan *s,
uint32_t var)
{
    int vi = fs_var_index(s, var);
    if (vi < 0)
    {
        STEREO_LOG(
            "FS_BINDING_LOOKUP_FAIL var=%u",
            var);
        return false;
    }
    const FsVariableInfo *v = &s->vars[vi];
    STEREO_LOG(
        "FS_BINDING_INFO var=%u type=%u storage=%u set=%u binding=%u",
        v->id,
        v->type,
        v->storage,
        v->set,
        v->binding);
    /*
     * Input attachments are framebuffer attachments.
     */
    if (v->storage == SpvStorageClassInput)
    {
        STEREO_LOG(
            "FS_BINDING_INPUT_ATTACHMENT var=%u stereo=1",
            var);
        return true;
    }
    /*
     * Deferred rendering attachments:
     *
     * binding 0 = depth/position
     * binding 1 = normal
     * binding 2 = albedo
     * binding 3 = specular
     * binding 4 = SSAO/deferred intermediate
     */
    bool stereo =
        (v->binding <= 4);
    STEREO_LOG(
        "FS_BINDING_RESULT var=%u binding=%u stereo=%u",
        var,
        v->binding,
        stereo);
    return stereo;
}

static bool
fs_should_patch_sample(
    const FsScan *s,
    uint64_t spv_hash,
    uint32_t descriptor_var)
{
    int vi = fs_var_index(s, descriptor_var);
    if (vi < 0)
        return false;

    uint32_t binding = s->vars[vi].binding;

    /*
     * SSAO noise is a mono lookup texture.  In the SSAO generator shader
     * (35d504ebec7cf2d7) it must not be arrayed or ViewIndex-shifted.
     */
    if (spv_hash == 0x35d504ebec7cf2d7ULL && binding == 2)
    {
        STEREO_LOG(
            "FS_SAMPLE_SKIP_NOISE hash=%016llx descriptor=%u binding=%u",
            (unsigned long long)spv_hash,
            descriptor_var,
            binding);
        return false;
    }

    return fs_binding_is_stereo_attachment(s, descriptor_var);
}

/*
 * Human-readable SPIR-V opcode names used only for diagnostics.
 * Keep this table small and focused on image/texture operations that
 * the fragment shader patcher cares about.
 */
static const char *
spv_op_name(uint32_t op)
{
    switch (op)
    {
    case SpvOpCopyObject:
        return "OpCopyObject";
    case SpvOpVariable:
        return "OpVariable";
    case SpvOpLoad:
        return "OpLoad";
    case SpvOpSampledImage:
        return "OpSampledImage";
    case SpvOpImage:
        return "OpImage";
    case SpvOpImageSampleImplicitLod:
        return "OpImageSampleImplicitLod";
    case SpvOpImageSampleExplicitLod:
        return "OpImageSampleExplicitLod";
    case SpvOpImageSampleDrefImplicitLod:
        return "OpImageSampleDrefImplicitLod";
    case SpvOpImageSampleDrefExplicitLod:
        return "OpImageSampleDrefExplicitLod";
    case SpvOpImageGather:
        return "OpImageGather";
    case SpvOpImageDrefGather:
        return "OpImageDrefGather";
    case SpvOpImageFetch:
        return "OpImageFetch";
    case SpvOpImageRead:
        return "OpImageRead";
    case SpvOpImageWrite:
        return "OpImageWrite";
    case SpvOpImageSparseSampleImplicitLod:
        return "OpImageSparseSampleImplicitLod";
    case SpvOpImageSparseSampleExplicitLod:
        return "OpImageSparseSampleExplicitLod";
    case SpvOpImageSparseFetch:
        return "OpImageSparseFetch";
    case SpvOpImageSparseRead:
        return "OpImageSparseRead";
    case SpvOpImageSparseTexelsResident:
        return "OpImageSparseTexelsResident";
    case SpvOpFunctionParameter:
        return "OpFunctionParameter";
    default:
        return "Unknown";
    }
}

static bool
fs_is_image_related_type(
    const FsScan *s,
    uint32_t type)
{
    if (!s)
        return false;

    for (uint32_t i = 0; i < s->n_img; ++i)
    {
        if (s->images[i].id == type)
            return true;
    }

    for (uint32_t i = 0; i < s->n_si; ++i)
    {
        if (s->si_ids[i] == type)
            return true;
    }

    return false;
}

/*═══════════════════════════════════════════════════════════════════════
 * FsScan lookup helpers
 *
 * These provide a single implementation for common table lookups used
 * throughout the fragment shader parser.
 *═══════════════════════════════════════════════════════════════════════*/
static int
fs_find_function(
    const FsScan *s,
    uint32_t function_id)
{
    if (!s)
        return -1;
    for (uint32_t i = 0; i < s->n_function; ++i)
    {
        if (s->functions[i].id == function_id)
            return (int)i;
    }
    return -1;
}
static int
fs_find_load(
    const FsScan *s,
    uint32_t value_id)
{
    if (!s)
        return -1;
    for (uint32_t i = 0; i < s->n_load; ++i)
    {
        if (s->loads[i].id == value_id)
            return (int)i;
    }
    return -1;
}
static int
fs_find_parameter(
    const FsScan *s,
    uint32_t parameter_id)
{
    if (!s)
        return -1;
    for (uint32_t i = 0; i < s->n_param; ++i)
    {
        if (s->params[i].id == parameter_id)
            return (int)i;
    }
    return -1;
}
static int
fs_find_call_parameter(
    const FsScan *s,
    uint32_t parameter_id)
{
    if (!s)
        return -1;
    for (uint32_t i = 0; i < s->n_call; ++i)
    {
        if (s->calls[i].parameter_id == parameter_id)
            return (int)i;
    }
    return -1;
}
/*═══════════════════════════════════════════════════════════════════════
 * Ownership helpers
 *═══════════════════════════════════════════════════════════════════════*/
/*
 * Record ownership of an SSA value that ultimately represents an image,
 * sampled image, or image object.
 *
 * The mapping is:
 *
 *     SSA value  --->  descriptor variable
 *
 * If the SSA value is already known (for example after propagation through
 * OpImage or OpCopyObject), simply update the owner instead of creating a
 * duplicate entry.
 */
static void
fs_add_load_mapping(
    FsScan *s,
    uint32_t value_id,
    uint32_t owner)
{
    if (!s)
        return;
    int index =
        fs_find_load(
            s,
            value_id);
    if (index >= 0)
    {
        s->loads[index].owner_var = owner;
        STEREO_LOG(
            "FS_LOAD_UPDATE id=%u owner=%u index=%d",
            value_id,
            owner,
            index);
        return;
    }
    if (s->n_load >= FS_MAX_LOADS)
    {
        STEREO_LOG(
            "FS_LOAD_OVERFLOW id=%u",
            value_id);
        return;
    }
    FsLoadInfo *load =
        &s->loads[s->n_load++];
    memset(
        load,
        0,
        sizeof(*load));
    load->id =
        value_id;
    load->owner_var =
        owner;
    load->source_id =
        value_id;
    load->binding =
        0xffffffffu;
    STEREO_LOG(
        "FS_LOAD_ADD id=%u owner=%u index=%u",
        value_id,
        owner,
        s->n_load - 1);
}
/*
 * Resolve the descriptor variable that owns a given SSA image value.
 *
 * The load table records ownership propagation through image-producing
 * instructions (OpLoad → OpImage → OpCopyObject → OpSampledImage, etc.).
 *
 * Returns true if ownership is known.
 */
static bool
fs_resolve_load_owner(
    const FsScan *s,
    uint32_t value_id,
    uint32_t *owner)
{
    if (!s)
        return false;
    int index =
        fs_find_load(
            s,
            value_id);
    if (index < 0)
    {
        STEREO_LOG(
            "FS_OWNER_LOOKUP_MISS value=%u",
            value_id);
        return false;
    }
    if (owner)
    {
        *owner =
            s->loads[index].owner_var;
    }
    STEREO_LOG(
        "FS_OWNER_LOOKUP value=%u owner=%u index=%d",
        value_id,
        s->loads[index].owner_var,
        index);
    return true;
}
/*═══════════════════════════════════════════════════════════════════════
 * Instruction scanners
 *═══════════════════════════════════════════════════════════════════════*/
/*═══════════════════════════════════════════════════════════════════════
 * Pass 1: Scan global SPIR-V types.
 *
 * This pass records:
 *
 *   • capabilities
 *   • entry point location
 *   • scalar/vector types
 *   • candidate image types
 *   • sampled-image types
 *   • ViewIndex input pointer type
 *
 * It intentionally does NOT determine whether an image will actually be
 * patched.  Descriptor bindings are not known yet.
 *═══════════════════════════════════════════════════════════════════════*/
static void
fs_scan_type_instruction(
    FsScan *s,
    const uint32_t *ins,
    uint32_t op,
    uint32_t wc)
{
    if (!s || !ins)
        return;
    //if (wc >= 2)
    //{
    //    STEREO_LOG(
    //        "FS_TYPE_DECL id=%u opcode=%u",
    //        ins[1],
    //        op);
    //}
    switch (op)
    {
    case SpvOpTypeFloat:
        if (wc >= 3 && ins[2] == 32)
        {
            s->float_id = ins[1];
        }
        break;
    case SpvOpTypeInt:
        if (wc >= 4 &&
            ins[2] == 32 &&
            ins[3] == 1)
        {
            s->int_id = ins[1];
        }
        break;
    case SpvOpTypeVector:
        if (wc >= 4 &&
            s->float_id &&
            ins[2] == s->float_id &&
            ins[3] == 3)
        {
            s->v3float_id = ins[1];
        }
        break;
    case SpvOpTypeImage:
    {
        if (wc < 9)
            break;
        uint32_t type_id = ins[1];
        uint32_t dim     = ins[3];
        uint32_t depth   = ins[4];
        uint32_t arrayed = ins[5];
        //STEREO_LOG(
        //    "FS_IMAGE_TYPE id=%u sampledType=%u dim=%u depth=%u arrayed=%u ms=%u sampled=%u format=%u",
        //    type_id,
        //    ins[2],
        //    dim,
        //    depth,
        //    arrayed,
        //    ins[6],
        //    ins[7],
        //    ins[8]);
        if (dim == SpvDim2D &&
            arrayed == 0 &&
            s->n_img < FS_MAX_IMG)
        {
            FsImageInfo *img =
                &s->images[s->n_img++];
            memset(img, 0, sizeof(*img));
            img->id             = type_id;
            img->depth          = depth;
            img->arrayed        = arrayed;
            img->patchable      = true;
            img->sampled_type   = 0;
            //STEREO_LOG(
            //    "FS_IMAGE_CANDIDATE type=%u depth=%u index=%u",
            //    img->id,
            //    img->depth,
            //    s->n_img - 1);
        }
        //else
        //{
        //    STEREO_LOG(
        //        "FS_IMAGE_REJECT type=%u dim=%u arrayed=%u",
        //        type_id,
        //        dim,
        //        arrayed);
        //}
        break;
    }
    case SpvOpTypeSampledImage:
    {
        STEREO_LOG(
            "FS_TYPE_SAMPLED_IMAGE id=%u imageType=%u",
            ins[1],
            (wc >= 3) ? ins[2] : 0);
        if (wc < 3)
            break;
        bool found = false;
        for (uint32_t i = 0; i < s->n_img; ++i)
        {
            if (s->images[i].id == ins[2])
            {
                s->images[i].sampled_type = ins[1];
                STEREO_LOG(
                    "FS_TYPE_SAMPLED_IMAGE_MAP imageType=%u sampledType=%u",
                    s->images[i].id,
                    s->images[i].sampled_type);
                found = true;
                break;
            }
        }
        if (found && s->n_si < FS_MAX_SI)
        {
            s->si_ids[s->n_si++] = ins[1];
            //STEREO_LOG(
            //    "FS_SAMPLED_IMAGE_LINK sampledType=%u imageType=%u",
            //    ins[1],
            //    ins[2]);
            //STEREO_LOG(
            //    "FS_SAMPLED_IMAGE_TYPE id=%u imageType=%u",
            //    ins[1],
            //    ins[2]);
        }
        break;
    }
    case SpvOpTypePointer:
        STEREO_LOG(
            "FS_TYPE_POINTER id=%u storage=%u target=%u",
            (wc >= 2) ? ins[1] : 0,
            (wc >= 3) ? ins[2] : 0,
            (wc >= 4) ? ins[3] : 0);
        if (wc >= 4 &&
            ins[2] == SpvStorageClassInput &&
            s->int_id &&
            ins[3] == s->int_id)
        {
            s->ptr_int_in_id = ins[1];
            STEREO_LOG(
                "FS_PTR_INT_INPUT id=%u",
                s->ptr_int_in_id);
        }
        break;
    default:
        break;
    }
}
/*
 * Process OpDecorate instructions.
 *
 * Descriptor decorations may appear before the corresponding
 * OpVariable declaration, so we cache them here and apply them
 * later when the variable is encountered.
 *
 * Handles:
 *   - Location
 *   - BuiltIn ViewIndex
 *   - Descriptor Binding
 *   - Descriptor Set
 *
 * Keeping this separate from fs_prescan() is important because
 * future projection-matrix handling will also need clean access
 * to descriptor metadata without depending on scan order.
 */
static void
fs_process_decoration(
    FsScan *s,
    const uint32_t *ins,
    uint32_t wc)
{
    if (!s || !ins || wc < 4)
        return;

    uint32_t target     = ins[1];
    uint32_t decoration = ins[2];
    uint32_t value      = ins[3];

    if (target == 15)
    {
        STEREO_LOG(
            "FS_DECORATION_VAR15 decoration=%u value=%u",
            decoration,
            value);
    }

    if (decoration == SpvDecorationBuiltIn &&
        value == SpvBuiltInViewIndex)
    {
        s->vi_var_id = target;
        STEREO_LOG(
            "FS_VIEWINDEX_FOUND id=%u",
            target);
        return;
    }

    if (decoration == SpvDecorationLocation)
    {
        int index = fs_var_index(s, target);
        if (index >= 0)
        {
            s->vars[index].location = value;
            STEREO_LOG(
                "FS_LOCATION_APPLY var=%u location=%u",
                target,
                value);
        }
        return;
    }

    if (decoration != SpvDecorationBinding &&
        decoration != SpvDecorationDescriptorSet)
    {
        return;
    }

    int index = -1;
    for (uint32_t i = 0; i < s->n_dec; ++i)
    {
        if (s->decorations[i].target == target)
        {
            index = (int)i;
            break;
        }
    }

    if (index < 0)
    {
        if (s->n_dec >= FS_MAX_VARS)
        {
            STEREO_LOG(
                "FS_DECORATION_OVERFLOW target=%u",
                target);
            return;
        }

        index = (int)s->n_dec++;
        FsDecorationInfo *dec = &s->decorations[index];
        memset(dec, 0, sizeof(*dec));
        dec->target   = target;
        dec->set      = 0xffffffffu;
        dec->binding  = 0xffffffffu;
        dec->location = 0xffffffffu;
    }

    FsDecorationInfo *dec = &s->decorations[index];
    if (decoration == SpvDecorationBinding)
        dec->binding = value;
    else
        dec->set = value;

    int var_index = fs_var_index(s, target);
    if (var_index >= 0)
    {
        FsVariableInfo *var = &s->vars[var_index];
        if (decoration == SpvDecorationBinding)
            var->binding = value;
        else
            var->set = value;

        if (var->storage == SpvStorageClassUniform &&
            var->binding == 4u)
        {
            var->is_projection_ubo = true;
            STEREO_LOG(
                "FS_PROJECTION_UBO_DECORATED var=%u set=%u binding=%u",
                var->id,
                var->set,
                var->binding);
        }
    }
}
/*═══════════════════════════════════════════════════════════════════════
 * Pass 2: Descriptor variables and decorations.
 *
 * Decorations (Binding, DescriptorSet, BuiltIn, Location) may legally
 * appear either before or after OpVariable, so this pass maintains a
 * temporary decoration cache which is applied whenever the matching
 * variable declaration is encountered.
 *
 * This pass does NOT determine whether a descriptor is stereo.
 * That decision happens later after image ownership is known.
 *═══════════════════════════════════════════════════════════════════════*/
static void
fs_scan_variable_instruction(
    FsScan *s,
    const uint32_t *ins,
    uint32_t wc)
{
    if (!s || !ins || wc < 4)
        return;

    if (s->n_var >= FS_MAX_VARS)
    {
        STEREO_LOG(
            "FS_VAR_OVERFLOW id=%u",
            ins[2]);
        return;
    }

    FsVariableInfo *var = &s->vars[s->n_var++];
    memset(var, 0, sizeof(*var));

    var->id       = ins[2];
    var->type     = ins[1];
    var->storage  = ins[3];
    var->set      = 0xffffffffu;
    var->binding  = 0xffffffffu;
    var->location = 0xffffffffu;
    var->is_projection_ubo = false;

    /*
     * Decorations may legally appear before OpVariable.
     * Apply cached DescriptorSet, Binding, and Location values now.
     */
    for (uint32_t i = 0; i < s->n_dec; ++i)
    {
        FsDecorationInfo *dec = &s->decorations[i];
        if (dec->target != var->id)
            continue;

        if (dec->set != 0xffffffffu)
            var->set = dec->set;
        if (dec->binding != 0xffffffffu)
            var->binding = dec->binding;
        if (dec->location != 0xffffffffu)
            var->location = dec->location;
    }

    if (var->storage == SpvStorageClassUniform &&
        var->binding == 4u)
    {
        var->is_projection_ubo = true;
        STEREO_LOG(
            "FS_PROJECTION_UBO var=%u type=%u set=%u binding=%u",
            var->id,
            var->type,
            var->set,
            var->binding);
    }

    if (var->id == 15)
    {
        STEREO_LOG(
            "FS_DEBUG_VAR15 type=%u storage=%u set=%u binding=%u location=%u",
            var->type,
            var->storage,
            var->set,
            var->binding,
            var->location);
    }

    if (var->storage == SpvStorageClassUniform ||
        var->storage == SpvStorageClassUniformConstant)
    {
        STEREO_LOG(
            "FS_DESCRIPTOR_VAR id=%u type=%u storage=%u set=%u binding=%u",
            var->id,
            var->type,
            var->storage,
            var->set,
            var->binding);
    }
}
/*
 * Register an OpVariable instruction.
 *
 * Variables are the bridge between:
 *
 *     sampled image objects
 *             |
 *             v
 *     descriptor variables
 *             |
 *             v
 *     descriptor set / binding
 *
 * Decorations may legally appear before OpVariable, so after
 * registering the variable we apply any cached metadata from
 * the decoration table.
 *
 * This function intentionally does NOT classify stereo resources.
 * Classification belongs later, after image provenance has been
 * resolved.
 */
static void
fs_scan_function_parameter(
    FsScan *s,
    const uint32_t *ins,
    uint32_t wc)
{
    if (wc < 3)
        return;
    if (!s->in_function)
    {
        STEREO_LOG(
            "FS_PARAM_OUTSIDE_FUNCTION id=%u",
            ins[2]);
        return;
    }
    if (s->n_param >= FS_MAX_PARAMS)
    {
        STEREO_LOG(
            "FS_PARAM_OVERFLOW id=%u",
            ins[2]);
        return;
    }
    uint32_t idx =
        s->n_param++;
    s->params[idx].id =
        ins[2];
    s->params[idx].type =
        ins[1];
    s->params[idx].function_id =
        s->current_function_id;
    s->params[idx].index =
        s->current_param_index;
    STEREO_LOG(
        "FS_PARAM_ADD function=%u index=%u id=%u type=%u",
        s->current_function_id,
        s->current_param_index,
        ins[2],
        ins[1]);
    /*
     * Parameters do not own descriptors directly.
     *
     * Example:
     *
     *   OpFunctionParameter %image %param
     *
     * The actual descriptor ownership is resolved later through:
     *
     *   OpFunctionCall arguments
     *          |
     *          v
     *   parameter id
     *          |
     *          v
     *   originating descriptor variable
     *
     * This deferred resolution is required for deferred renderers
     * where image sampling happens inside helper functions.
     */
    s->current_param_index++;
}
/*
 * Scan function structure.
 *
 * Responsibilities:
 *  - track current function
 *  - remember first function offset
 *  - record every function parameter
 *  - build lookup tables used later to resolve descriptor ownership
 *
 * This performs no image analysis; it only records function metadata.
 */
static void
fs_scan_function(
    FsScan *s,
    const uint32_t *ins,
    uint32_t wc)
{
    if (!s || !ins || wc < 3)
        return;
    uint32_t function_id =
        ins[2];
    /*
     * Track current function context while scanning.
     *
     * SPIR-V functions can contain parameters that appear before
     * the actual image operations using them.  We therefore record
     * function ownership first, then resolve argument forwarding
     * later in fs_fixup_function_parameters().
     */
    s->in_function = true;
    s->current_function_id = function_id;
    s->current_param_index = 0;
    if (s->n_function >= FS_MAX_FUNCTIONS)
    {
        STEREO_LOG(
            "FS_FUNCTION_OVERFLOW id=%u",
            function_id);
        return;
    }
    FsFunctionInfo *fn =
        &s->functions[s->n_function++];
    fn->id =
        function_id;
    fn->first_param =
        s->n_param;
    STEREO_LOG(
        "FS_FUNCTION_REGISTER id=%u index=%u firstParam=%u",
        function_id,
        s->n_function - 1,
        fn->first_param);
    STEREO_LOG(
        "FS_FUNCTION_BEGIN id=%u",
        function_id);
}
/*
 * Track OpLoad instructions that produce image-related objects.
 *
 * SPIR-V image usage commonly looks like:
 *
 *     OpLoad          %image   %descriptor
 *     OpSampledImage  %sampled %image %sampler
 *     OpImageSample   ...
 *
 * We cannot classify the image immediately because:
 *
 *   - descriptor decorations may appear later
 *   - function parameters may hide the originating variable
 *   - image objects can be copied through intermediate IDs
 *
 * Therefore this function only records provenance.
 *
 * Later passes resolve:
 *   image ID -> descriptor variable -> set/binding
 */
static void
fs_scan_load_instruction(
    FsScan *s,
    const uint32_t *ins,
    uint32_t wc)
{
    if (!s || !ins || wc < 4)
        return;

    uint32_t result_type = ins[1];
    uint32_t result_id   = ins[2];
    uint32_t source_id   = ins[3];

    uint32_t owner = 0;
    bool have_owner = fs_resolve_load_owner(s, source_id, &owner);

    if (!have_owner)
    {
        /*
         * The source may be a function parameter or another SSA value
         * that will be fixed up later.
         */
        owner = source_id;
        STEREO_LOG(
            "FS_LOAD_DEFERRED result=%u source=%u",
            result_id,
            source_id);
    }

    int owner_var_index = fs_var_index(s, owner);
    bool from_projection_ubo =
        (owner_var_index >= 0 &&
         s->vars[owner_var_index].is_projection_ubo);

    bool image_related =
        fs_is_image_related_type(s, result_type);

    /*
     * Keep tracking normal image-related loads as before.
     * Also keep projection UBO loads even when they are not image-related,
     * because the FS uses them for convergence/projection reconstruction.
     */
    if (!image_related && !from_projection_ubo)
        return;

    if (s->n_load >= FS_MAX_LOADS)
    {
        STEREO_LOG(
            "FS_LOAD_OVERFLOW result=%u",
            result_id);
        return;
    }

    FsLoadInfo *li = &s->loads[s->n_load++];
    memset(li, 0, sizeof(*li));

    li->id            = result_id;
    li->source_id     = source_id;
    li->owner_var     = owner;
    li->binding       = 0xffffffffu;
    li->from_projection = from_projection_ubo;
    li->from_view       = false;

    if (owner_var_index >= 0)
    {
        li->binding = s->vars[owner_var_index].binding;
    }

    int src_var = fs_var_index(s, source_id);
    if (src_var >= 0)
    {
        STEREO_LOG(
            "FS_LOAD_SOURCE result=%u sourceVar=%u set=%u binding=%u type=%u proj=%u",
            result_id,
            source_id,
            s->vars[src_var].set,
            s->vars[src_var].binding,
            s->vars[src_var].type,
            li->from_projection);
    }
    else
    {
        STEREO_LOG(
            "FS_LOAD_SOURCE_UNKNOWN result=%u source=%u",
            result_id,
            source_id);
    }

    if (from_projection_ubo)
    {
        STEREO_LOG(
            "FS_PROJECTION_LOAD result=%u owner=%u set=%u binding=%u type=%u",
            result_id,
            owner,
            (owner_var_index >= 0) ? s->vars[owner_var_index].set : 0xffffffffu,
            (owner_var_index >= 0) ? s->vars[owner_var_index].binding : 0xffffffffu,
            (owner_var_index >= 0) ? s->vars[owner_var_index].type : 0xffffffffu);
    }

    STEREO_LOG(
        "FS_LOAD_REGISTER result=%u owner=%u type=%u proj=%u",
        result_id,
        owner,
        result_type,
        li->from_projection);
}
/*
 * Track OpFunctionCall relationships.
 *
 * SPIR-V functions make descriptor ownership difficult because
 * a sampled image may flow through parameters:
 *
 *   main()
 *      |
 *      | OpFunctionCall
 *      v
 *   helper(image)
 *      |
 *      | OpFunctionParameter
 *      v
 *   OpLoad
 *
 * During the first scan we do not yet know every parameter owner.
 *
 * Therefore this function records:
 *
 *   function ID
 *   argument index
 *   argument value
 *
 * Later fs_fixup_function_parameters() resolves:
 *
 *   parameter ID -> original descriptor variable
 *
 * This separation keeps resource classification independent
 * from SPIR-V function ordering.
 */
static void
fs_scan_function_call(
    FsScan *s,
    const uint32_t *ins,
    uint32_t wc)
{
    if (!s || !ins || wc < 4)
        return;
    uint32_t result_id =
        ins[2];
    uint32_t function_id =
        ins[3];
    uint32_t argument_count =
        wc - 4;
    STEREO_LOG(
        "FS_FUNCTION_CALL result=%u function=%u argc=%u",
        result_id,
        function_id,
        argument_count);
    for (uint32_t arg = 0;
         arg < argument_count;
         ++arg)
    {
        uint32_t value =
            ins[4 + arg];
        STEREO_LOG(
            "FS_FUNCTION_ARG index=%u value=%u",
            arg,
            value);
        if (s->n_call >= FS_MAX_CALLS)
        {
            STEREO_LOG(
                "FS_CALL_OVERFLOW function=%u arg=%u",
                function_id,
                arg);
            continue;
        }
        FsCallInfo *call =
            &s->calls[s->n_call++];
        memset(
            call,
            0,
            sizeof(*call));
        call->function_id =
            function_id;
        /*
         * During the first scan this is the argument position.
         * fs_fixup_function_parameters() converts it into the
         * real parameter ID after the function table is known.
         */
        call->parameter_index =
            arg;
        call->argument_var =
            value;
        call->parameter_id =
            0;
        STEREO_LOG(
            "FS_CALL_STORE function=%u argIndex=%u value=%u total=%u",
            function_id,
            arg,
            value,
            s->n_call);
    }
}
/*
 * Ownership propagation
 */
/*
 * Propagate descriptor ownership through image-producing instructions.
 *
 * Many deferred renderers perform chains such as:
 *
 *      OpLoad
 *          ↓
 *      OpImage
 *          ↓
 *      OpCopyObject
 *          ↓
 *      OpImageSample*
 *
 * Each intermediate SSA value must retain the descriptor ownership of the
 * original OpLoad so later sampling instructions can still recover the
 * descriptor binding.
 */
static void
fs_track_image_propagation(
    FsScan *s,
    const uint32_t *ins,
    uint32_t op,
    uint32_t wc)
{
    if (!s || wc < 4)
        return;
    bool propagate =
        (op == SpvOpImage) ||
        (op == SpvOpCopyObject);
    if (!propagate)
        return;
    uint32_t result_id = ins[2];
    uint32_t source_id = ins[3];
    int src =
        fs_find_load(
            s,
            source_id);
    if (src < 0)
    {
        return;
    }
    if (s->n_load >= FS_MAX_LOADS)
    {
        STEREO_LOG(
            "FS_PROP_OVERFLOW result=%u",
            result_id);
        return;
    }
    FsLoadInfo *dst =
        &s->loads[s->n_load++];
    *dst = s->loads[src];
    dst->id = result_id;
    STEREO_LOG(
        "FS_PROPAGATE op=%s src=%u dst=%u owner=%u source=%u",
        spv_op_name(op),
        source_id,
        result_id,
        dst->owner_var,
        dst->source_id);
}
/*
 * Track OpSampledImage ownership.
 *
 * OpSampledImage combines:
 *
 *      image object
 *          +
 *      sampler object
 *
 * into a sampled-image object consumed by OpImageSample*.
 *
 * We only care about preserving the descriptor ownership of the image
 * object so later texture sampling instructions can still recover the
 * originating descriptor binding.
 */
static void
fs_track_sampled_image(
    FsScan *s,
    const uint32_t *ins,
    uint32_t wc)
{
    if (!s || wc < 5)
        return;
    /*
     * Ignore non-sampled-image result types.
     */
    if (!fs_id_in(
            s->si_ids,
            s->n_si,
            ins[1]))
    {
        return;
    }
    uint32_t result_id  = ins[2];
    uint32_t image_id   = ins[3];
    uint32_t sampler_id = ins[4];
    int src =
        fs_find_load(
            s,
            image_id);
    if (src < 0)
    {
        STEREO_LOG(
            "FS_SAMPLED_IMAGE_NO_SOURCE result=%u image=%u sampler=%u",
            result_id,
            image_id,
            sampler_id);
        return;
    }
    if (s->n_load >= FS_MAX_LOADS)
    {
        STEREO_LOG(
            "FS_SAMPLED_IMAGE_OVERFLOW result=%u",
            result_id);
        return;
    }
    FsLoadInfo *dst =
        &s->loads[s->n_load++];
    *dst = s->loads[src];
    dst->id = result_id;
    STEREO_LOG(
        "FS_SAMPLED_IMAGE result=%u image=%u sampler=%u owner=%u source=%u",
        result_id,
        image_id,
        sampler_id,
        dst->owner_var,
        dst->source_id);
}
/*
 * Scan image sampling/fetch/read/write instructions.
 *
 * At this point we do not modify these instructions. The goal is to:
 *
 *   • determine which descriptor is being sampled
 *   • log the descriptor binding
 *   • verify ownership propagation worked correctly
 *
 * The actual SPIR-V rewriting happens later in the patch pass.
 */
static void
fs_scan_image_operation(
    FsScan *s,
    const uint32_t *ins,
    uint32_t op,
    uint32_t wc)
{
    STEREO_LOG(
        "FS_IMAGE_SCAN op=%s imageOperand=%u resultType=%u result=%u",
        spv_op_name(op),
        (wc >= 4) ? ins[3] : 0,
        (wc >= 2) ? ins[1] : 0,
        (wc >= 3) ? ins[2] : 0);
    if (!s || wc < 5)
        return;
    switch (op)
    {
    case SpvOpImageSampleImplicitLod:
    case SpvOpImageSampleExplicitLod:
    case SpvOpImageSampleDrefImplicitLod:
    case SpvOpImageSampleDrefExplicitLod:
    case SpvOpImageFetch:
    case SpvOpImageRead:
    case SpvOpImageWrite:
        STEREO_LOG(
            "FS_IMAGE_WRITE image=%u value=%u",
            ins[3],
            ins[4]);
        break;
    default:
        return;
    }
    uint32_t image_id = ins[3];
    int load =
        fs_find_load(
            s,
            image_id);
    if (load < 0)
    {
        STEREO_LOG(
            "FS_IMAGE_NO_LOAD image=%u op=%s",
            image_id,
            spv_op_name(op));
        return;
    }
    FsLoadInfo *li =
        &s->loads[load];
    if (li->owner_var == 0)
    {
        STEREO_LOG(
            "FS_IMAGE_UNRESOLVED image=%u source=%u",
            image_id,
            li->source_id);
        return;
    }
    int var =
        fs_var_index(
            s,
            li->owner_var);
    if (var < 0)
    {
        STEREO_LOG(
            "FS_IMAGE_OWNER_UNKNOWN owner=%u",
            li->owner_var);
        return;
    }
    bool stereo =
        fs_binding_is_stereo_attachment(
            s,
            li->owner_var);
    STEREO_LOG(
        "FS_SAMPLE_CLASSIFIED image=%u owner=%u binding=%u stereo=%u op=%s",
        image_id,
        li->owner_var,
        s->vars[var].binding,
        stereo,
        spv_op_name(op));
    li->binding =
        s->vars[var].binding;
    STEREO_LOG(
        "FS_IMAGE_SAMPLE op=%s image=%u owner=%u set=%u binding=%u stereo=%u proj=%u view=%u",
        spv_op_name(op),
        image_id,
        li->owner_var,
        s->vars[var].set,
        s->vars[var].binding,
        stereo,
        li->from_projection,
        li->from_view);
    if (stereo)
    {
        STEREO_LOG(
            "FS_STEREO_RESOURCE image=%u binding=%u proj=%u",
            image_id,
            s->vars[var].binding,
            li->from_projection);
    }
    STEREO_LOG(
        "FS_IMAGE_OWNER image=%u owner=%u",
        (wc >= 4) ? ins[3] : 0,
        li->owner_var);
}
/*
 * Instruction dispatchers
 * ----------------------
 * Module traversal lives here. The individual semantic handlers above should
 * never walk the SPIR-V stream themselves.
 */
/*
 * Scan one SPIR-V instruction.
 *
 * This dispatcher performs the semantic analysis pass used by the
 * fullscreen-fragment patcher. Each instruction category is handled by a
 * dedicated helper so fs_prescan() only performs module traversal.
 *
 * No SPIR-V is modified here; this pass only records metadata needed by the
 * later patching phase.
 */
static void
fs_scan_instruction(
    FsScan *s,
    const uint32_t *ins,
    uint32_t op,
    uint32_t wc)
{
    if (!s || !ins)
        return;
    /*
     * Diagnostic: log every image operation encountered during
     * the prescan so we know exactly which SPIR-V instructions
     * this shader uses for MSAA resolve/final composition.
     */
    switch (op)
    {
    case SpvOpImageSampleImplicitLod:
    case SpvOpImageSampleExplicitLod:
    case SpvOpImageSampleDrefImplicitLod:
    case SpvOpImageSampleDrefExplicitLod:
    case SpvOpImageFetch:
    case SpvOpImageRead:
    case SpvOpImageWrite:
        STEREO_LOG(
            "FS_IMAGE_OP opcode=%u wc=%u result=%u image=%u",
            op,
            wc,
            (wc >= 3) ? ins[2] : 0,
            (wc >= 4) ? ins[3] : 0);
        break;
    default:
        break;
    }
    switch (op)
    {
    /*
     * Type declarations.
     *
     * These must be scanned before variables because
     * later resource classification depends on knowing
     * image/sampled-image relationships.
     */
    case SpvOpTypeFloat:
    case SpvOpTypeInt:
    case SpvOpTypeVector:
    case SpvOpTypeImage:
    case SpvOpTypeSampledImage:
    case SpvOpTypePointer:
        fs_scan_type_instruction(
            s,
            ins,
            op,
            wc);
        break;
    /*
     * Decorations may appear before OpVariable.
     *
     * Cache them first and apply them when the variable
     * is encountered.
     */
    case SpvOpDecorate:
        fs_process_decoration(
            s,
            ins,
            wc);
        break;
    /*
     * Descriptor/resource declarations.
     */
    case SpvOpVariable:
        fs_scan_variable_instruction(
            s,
            ins,
            wc);
        break;
    /*
     * Function metadata.
     */
    case SpvOpFunction:
        fs_scan_function(
            s,
            ins,
            wc);
        break;
    case SpvOpFunctionParameter:
        fs_scan_function_parameter(
            s,
            ins,
            wc);
        break;
    case SpvOpFunctionEnd:
        s->in_function = false;
        s->current_function_id = 0;
        s->current_param_index = 0;
        STEREO_LOG(
            "FS_FUNCTION_END");
        break;
    case SpvOpFunctionCall:
        STEREO_LOG(
            "FS_FUNCTION_CALL wc=%u resultType=%u result=%u function=%u",
            wc,
            wc > 1 ? ins[1] : 0,
            wc > 2 ? ins[2] : 0,
            wc > 3 ? ins[3] : 0);
        fs_scan_function_call(
            s,
            ins,
            wc);
        break;
    /*
     * Resource ownership tracking.
     *
     * Keep SSA ownership for:
     *  - direct loads
     *  - pointer arithmetic / access chains
     *  - simple forwarding ops
     *
     * This is required so a later OpLoad from an access chain can still
     * be traced back to the originating uniform variable.
     */
    case SpvOpAccessChain:
    case SpvOpInBoundsAccessChain:
    case SpvOpPtrAccessChain:
    {
        if (wc >= 4)
        {
            uint32_t result_id = ins[2];
            uint32_t base_id   = ins[3];
            uint32_t owner     = base_id;
            if (!fs_resolve_load_owner(s, base_id, &owner))
            {
                if (fs_var_index(s, base_id) >= 0)
                    owner = base_id;
            }
            fs_add_load_mapping(s, result_id, owner);
            STEREO_LOG(
                "FS_CHAIN result=%u base=%u owner=%u op=%s",
                result_id,
                base_id,
                owner,
                spv_op_name(op));
        }
        break;
    }
    case SpvOpCopyObject:
    case SpvOpBitcast:
    {
        if (wc >= 4)
        {
            uint32_t result_id = ins[2];
            uint32_t source_id = ins[3];
            uint32_t owner     = source_id;
            if (!fs_resolve_load_owner(s, source_id, &owner))
            {
                if (fs_var_index(s, source_id) >= 0)
                    owner = source_id;
            }
            fs_add_load_mapping(s, result_id, owner);
            STEREO_LOG(
                "FS_PROPAGATE_OBJECT op=%s src=%u dst=%u owner=%u",
                spv_op_name(op),
                source_id,
                result_id,
                owner);
        }
        break;
    }
    case SpvOpCompositeExtract:
    {
        if (wc >= 5)
        {
            uint32_t result_id = ins[2];
            uint32_t source_id = ins[3];
            uint32_t owner     = source_id;
            if (!fs_resolve_load_owner(s, source_id, &owner))
            {
                if (fs_var_index(s, source_id) >= 0)
                    owner = source_id;
            }
            fs_add_load_mapping(s, result_id, owner);
        }
        break;
    }
    case SpvOpVectorShuffle:
    {
        if (wc >= 6)
        {
            uint32_t result_id = ins[2];
            uint32_t source_id = ins[3];
            uint32_t owner     = source_id;
            if (!fs_resolve_load_owner(s, source_id, &owner))
            {
                if (fs_var_index(s, source_id) >= 0)
                    owner = source_id;
            }
            fs_add_load_mapping(s, result_id, owner);
        }
        break;
    }
    case SpvOpLoad:
        fs_scan_load_instruction(
            s,
            ins,
            wc);
        break;
    case SpvOpSampledImage:
    {
        if (wc >= 5)
        {
            STEREO_LOG(
                "FS_SAMPLED_IMAGE_OP resultType=%u result=%u image=%u sampler=%u",
                ins[1],
                ins[2],
                ins[3],
                ins[4]);
        }
        fs_track_sampled_image(
            s,
            ins,
            wc);
        break;
    }
    case SpvOpImage:
        fs_track_image_propagation(
            s,
            ins,
            op,
            wc);
        break;
    /*
     * Final image consumers.
     *
     * This is where depth/normal attachment analysis
     * will eventually feed projection correction.
     */
    case SpvOpImageSampleImplicitLod:
    case SpvOpImageSampleExplicitLod:
    case SpvOpImageSampleDrefImplicitLod:
    case SpvOpImageSampleDrefExplicitLod:
    case SpvOpImageFetch:
    case SpvOpImageRead:
    case SpvOpImageWrite:
        fs_scan_image_operation(
            s,
            ins,
            op,
            wc);
        break;
    default:
        break;
    }
}
/*
 * fs_prescan()
 *
 * High-level SPIR-V prescan dispatcher.
 *
 * The old implementation mixed:
 *
 *   - type discovery
 *   - descriptor tracking
 *   - function analysis
 *   - image ownership propagation
 *   - debug tracing
 *
 * in one large loop.
 *
 * This wrapper keeps the scan order explicit while allowing each
 * analysis stage to remain independently debuggable.
 *
 * Scan order matters:
 *
 *  1. Types must be known before variables can be classified.
 *  2. Decorations may appear before OpVariable, so they are cached.
 *  3. Variables establish descriptor ownership.
 *  4. Function parameters/calls are collected.
 *  5. Loads and image operations propagate ownership.
 *  6. Post-pass fixups resolve deferred relationships.
 */
static void
fs_fixup_function_parameters(
    FsScan *s);
static void
fs_dump_scan_summary(
    const FsScan *s);
static void
fs_prescan(
    FsScan *s,
    const uint32_t *w,
    size_t c)
{
    STEREO_LOG("FS_PRESCAN_ENTER");
    if (!s || !w || c < 5)
    {
        STEREO_LOG(
            "FS_PRESCAN_ABORT s=%p w=%p size=%zu",
            s,
            w,
            c);
        return;
    }
    memset(
        s,
        0,
        sizeof(*s));
    /*
     * SPIR-V module layout:
     *
     *   [0] Magic
     *   [1] Version
     *   [2] Generator
     *   [3] Bound
     *   [4] Schema
     *
     * Instructions begin at word 5.
     */
    for (size_t i = 5; i < c;)
    {
        uint32_t word =
            w[i];
        uint32_t op =
            word & 0xffffu;
        uint32_t wc =
            word >> 16;
        if (wc == 0 ||
            i + wc > c)
        {
            STEREO_LOG(
                "FS_INVALID_INSTRUCTION offset=%zu wc=%u size=%zu",
                i,
                wc,
                c);
            break;
        }
        fs_scan_instruction(
            s,
            &w[i],
            op,
            wc);
        i += wc;
    }
    STEREO_LOG(
        "FS_PRESCAN_SCAN_DONE loads=%u calls=%u params=%u",
        s->n_load,
        s->n_call,
        s->n_param);
    /*
     * Resolve deferred parameter ownership.
     */
    fs_fixup_function_parameters(
        s);
    STEREO_LOG(
        "FS_PARAM_STATE params=%u calls=%u",
        s->n_param,
        s->n_call);
    
    for (uint32_t p = 0; p < s->n_param; ++p)
    {
        STEREO_LOG(
            "FS_PARAM id=%u index=%u",
            s->params[p].id,
            p);
    }
    for (uint32_t cidx = 0; cidx < s->n_call; ++cidx)
    {
        STEREO_LOG(
            "FS_CALL param=%u arg=%u",
            s->calls[cidx].parameter_id,
            s->calls[cidx].argument_var);
    }
    STEREO_LOG(
        "FS_PRESCAN_AFTER_FIXUP loads=%u calls=%u params=%u",
        s->n_load,
        s->n_call,
        s->n_param);
    /*
     * Rewrite loads that still reference function parameter IDs
     * to the caller's descriptor variable.
     */
    for (uint32_t l = 0; l < s->n_load; ++l)
    {
        FsLoadInfo *load =
            &s->loads[l];
        STEREO_LOG(
            "FS_LOAD_CHECK load=%u owner=%u",
            load->id,
            load->owner_var);
        for (uint32_t cidx = 0;
             cidx < s->n_call;
             ++cidx)
        {
            FsCallInfo *call =
                &s->calls[cidx];
            STEREO_LOG(
                "FS_CALL_CHECK param=%u arg=%u",
                call->parameter_id,
                call->argument_var);
            if (load->owner_var ==
                call->parameter_id)
            {
                STEREO_LOG(
                    "FS_LOAD_FINAL_RESOLVE load=%u param=%u owner=%u",
                    load->id,
                    load->owner_var,
                    call->argument_var);
                STEREO_LOG(
                    "FS_LOAD_RESOLVED load=%u owner=%u",
                    load->id,
                    call->argument_var);
                load->owner_var =
                    call->argument_var;
                break;
            }
        }
    }
    /*
     * Dump the final ownership graph after fixups.
     */
    fs_dump_scan_summary(s);
    /*
     * Resolve descriptor ownership for every image type.
     * This allows spirv_patch_stereo_fs() to know which binding
     * owns a given OpTypeImage.
     */
    for (uint32_t img = 0; img < s->n_img; ++img)
    {
        FsImageInfo *image = &s->images[img];
        image->owner_var = UINT32_MAX;
        image->binding   = UINT32_MAX;
        image->set       = UINT32_MAX;
        for (uint32_t v = 0; v < s->n_var; ++v)
        {
            if (image->sampled_type &&
                s->vars[v].type == image->sampled_type)
            {
                image->owner_var = s->vars[v].id;
                image->binding   = s->vars[v].binding;
                image->set       = s->vars[v].set;
                STEREO_LOG(
                    "FS_IMAGE_OWNER "
                    "imageType=%u "
                    "sampledType=%u "
                    "owner=%u "
                    "set=%u "
                    "binding=%u",
                    image->id,
                    image->sampled_type,
                    image->owner_var,
                    image->set,
                    image->binding);
                break;
            }
        }
    }
    for (uint32_t l = 0; l < s->n_load; ++l)
    {
        const FsLoadInfo *load = &s->loads[l];
        int vi = fs_var_index(s, load->owner_var);
        STEREO_LOG(
            "FS_FINAL_LOAD load=%u owner=%u set=%u binding=%u storage=%u type=%u",
            load->id,
            load->owner_var,
            (vi >= 0) ? s->vars[vi].set : 0xffffffffu,
            (vi >= 0) ? s->vars[vi].binding : 0xffffffffu,
            (vi >= 0) ? s->vars[vi].storage : 0xffffffffu,
            (vi >= 0) ? s->vars[vi].type : 0xffffffffu);
    }
    for (uint32_t v = 0; v < s->n_var; ++v)
    {
        if (s->vars[v].id == 15)
        {
            STEREO_LOG(
                "FS_VAR15_FINAL type=%u storage=%u set=%u binding=%u",
                s->vars[v].type,
                s->vars[v].storage,
                s->vars[v].set,
                s->vars[v].binding);
        }
    }
    STEREO_LOG(
        "FS_PRESCAN_EXIT");
}
/*
 * fs_prescan_module()
 *
 * Entry point for fragment shader analysis.
 *
 * Responsibilities:
 *
 *   - initialize FsScan state
 *   - perform the SPIR-V instruction scan
 *   - resolve deferred relationships
 *   - emit final diagnostics
 *
 * Keeping this wrapper separate from fs_prescan() allows future
 * multi-pass analysis:
 *
 *   Pass 1:
 *       structural discovery
 *
 *   Pass 2:
 *       descriptor/image provenance
 *
 *   Pass 3:
 *       projection/depth-space classification
 *
 * The future projection-matrix system will use this separation to
 * determine whether a shader samples:
 *
 *   - camera-space data
 *   - screen-space buffers
 *   - depth reconstructed positions
 *   - lighting/deferred intermediates
 */
static bool
fs_prescan_module(
    FsScan *s,
    const uint32_t *w,
    size_t c)
{
    if (!s || !w || c < 5)
    {
        STEREO_LOG(
            "FS_PRESCAN_INVALID_MODULE");
        return false;
    }
    fs_prescan(
        s,
        w,
        c);
    if (s->n_var == 0 &&
        s->n_img == 0 &&
        s->n_load == 0)
    {
        STEREO_LOG(
            "FS_PRESCAN_EMPTY_MODULE");
    }
    STEREO_LOG(
        "FS_PRESCAN_COMPLETE vars=%u images=%u loads=%u functions=%u calls=%u",
        s->n_var,
        s->n_img,
        s->n_load,
        s->n_function,
        s->n_call);
    return true;
}
/*
 * Post-processing
 */
/*
 * Resolve descriptor ownership across function calls.
 *
 * During the initial scan we only know:
 *
 *     caller argument #0  ---> descriptor variable
 *
 * Later, after every function has been scanned, we know:
 *
 *     function parameter ID corresponding to argument #0
 *
 * This pass joins those two pieces of information so image loads
 * performed inside helper functions still resolve back to the
 * original descriptor variable.
 *
 * Before:
 *
 *      load_vars[] --> parameter ID
 *
 * After:
 *
 *      load_vars[] --> descriptor variable
 *
 * This is required because many deferred renderers wrap depth,
 * normal and SSAO sampling inside helper functions.
 */
static void
fs_fixup_function_parameters(
    FsScan *s)
{
    if (!s)
        return;
    /*
     * Resolve parameter ids for every call.
     */
    for (uint32_t i = 0; i < s->n_call; ++i)
    {
        FsCallInfo *call =
            &s->calls[i];
        int fn =
            fs_find_function(
                s,
                call->function_id);
        if (fn < 0)
            continue;
        FsFunctionInfo *func =
            &s->functions[fn];
        uint32_t param_index =
            func->first_param +
            call->parameter_index;
        if (param_index >= s->n_param)
            continue;
        call->parameter_id =
            s->params[param_index].id;
        STEREO_LOG(
            "FS_CALL_PARAMETER function=%u param=%u arg=%u",
            call->function_id,
            call->parameter_id,
            call->argument_var);
    }
    /*
     * Resolve deferred ownership.
     *
     * owner_var currently contains the parameter SSA id
     * recorded during OpLoad.
     */
    for (uint32_t i = 0; i < s->n_load; ++i)
    {
        FsLoadInfo *load =
            &s->loads[i];
        int p =
            fs_find_parameter(
                s,
                load->owner_var);
        if (p < 0)
            continue;
        FsParameterInfo *param =
            &s->params[p];
        bool resolved = false;
        for (uint32_t c = 0; c < s->n_call; ++c)
        {
            FsCallInfo *call =
                &s->calls[c];
            if (call->function_id !=
                param->function_id)
                continue;
            if (call->parameter_id !=
                param->id)
                continue;
            load->owner_var =
                call->argument_var;
            resolved = true;
            STEREO_LOG(
                "FS_LOAD_FIXUP load=%u owner=%u",
                load->id,
                load->owner_var);
            break;
        }
        if (!resolved)
        {
            STEREO_LOG(
                "FS_LOAD_FIXUP_FAILED load=%u param=%u",
                load->id,
                param->id);
        }
    }
    uint32_t unresolved = 0;
    for (uint32_t i = 0; i < s->n_load; ++i)
    {
        int var =
            fs_var_index(
                s,
                s->loads[i].owner_var);
        if (var < 0)
            ++unresolved;
    }
    STEREO_LOG(
        "FS_FIXUP_COMPLETE loads=%u unresolved=%u",
        s->n_load,
        unresolved);
}
/*
 * Dump the final prescan state.
 *
 * This is called after all instruction scanning and ownership
 * fixups have completed. At this point every descriptor,
 * function parameter and image load should have been resolved
 * to its originating descriptor variable.
 *
 * These logs are invaluable when diagnosing why a sampled image
 * was (or was not) classified as a stereo attachment.
 */
static void
fs_dump_scan_summary(
    const FsScan *s)
{
    if (!s)
        return;
    STEREO_LOG(
        "========== FS PRESCAN SUMMARY ==========");
    STEREO_LOG(
        "Images=%u SampledImages=%u Variables=%u Loads=%u Params=%u Functions=%u Calls=%u",
        s->n_img,
        s->n_si,
        s->n_var,
        s->n_load,
        s->n_param,
        s->n_function,
        s->n_call);
    /*
     * Image types
     */
    for (uint32_t i = 0; i < s->n_img; ++i)
    {
        const FsImageInfo *img =
            &s->images[i];
        STEREO_LOG(
            "FS_IMAGE_FINAL id=%u depth=%u arrayed=%u patchable=%u",
            img->id,
            img->depth,
            img->arrayed,
            img->patchable);
    }
    /*
     * Variables
     */
    for (uint32_t i = 0; i < s->n_var; ++i)
    {
        const FsVariableInfo *v =
            &s->vars[i];
        STEREO_LOG(
            "FS_VAR_FINAL id=%u type=%u storage=%u set=%u binding=%u location=%u",
            v->id,
            v->type,
            v->storage,
            v->set,
            v->binding,
            v->location);
    }
    /*
     * Decorations
     */
    for (uint32_t i = 0; i < s->n_dec; ++i)
    {
        const FsDecorationInfo *d =
            &s->decorations[i];
        STEREO_LOG(
            "FS_DEC target=%u set=%u binding=%u",
            d->target,
            d->set,
            d->binding);
    }
    /*
     * Functions
     */
    for (uint32_t i = 0; i < s->n_function; ++i)
    {
        const FsFunctionInfo *fn =
            &s->functions[i];
        STEREO_LOG(
            "FS_FUNCTION id=%u firstParam=%u",
            fn->id,
            fn->first_param);
    }
    /*
     * Parameters
     */
    for (uint32_t i = 0; i < s->n_param; ++i)
    {
        const FsParameterInfo *p =
            &s->params[i];
        STEREO_LOG(
            "FS_PARAM id=%u function=%u index=%u",
            p->id,
            p->function_id,
            p->index);
    }
    /*
     * Loads
     */
    for (uint32_t i = 0; i < s->n_load; ++i)
    {
        const FsLoadInfo *load =
            &s->loads[i];
        int var =
            fs_var_index(
                s,
                load->owner_var);
        if (var >= 0)
        {
            STEREO_LOG(
                "FS_LOAD_FINAL id=%u source=%u owner=%u set=%u binding=%u storage=%u",
                load->id,
                load->source_id,
                load->owner_var,
                s->vars[var].set,
                s->vars[var].binding,
                s->vars[var].storage);
        }
        else
        {
            STEREO_LOG(
                "FS_LOAD_FINAL id=%u source=%u owner=%u (unresolved)",
                load->id,
                load->source_id,
                load->owner_var);
        }
    }
    /*
     * Calls
     */
    for (uint32_t i = 0; i < s->n_call; ++i)
    {
        const FsCallInfo *call =
            &s->calls[i];
        STEREO_LOG(
            "FS_CALL_FINAL function=%u parameter=%u argument=%u",
            call->function_id,
            call->parameter_id,
            call->argument_var);
    }
    STEREO_LOG(
        "========================================");
}

static uint32_t
fs_count_patches(
    const FsScan *s,
    const uint32_t *w,
    size_t c)
{
    uint32_t count = 0;
    bool in_func = false;
    for (size_t i = 5; i < c;)
    {
        uint32_t op = w[i] & 0xffffu;
        uint32_t wc = w[i] >> 16;
        if (!wc || i + wc > c)
            break;
        if (op == SpvOpFunction)
            in_func = true;
        /*
         * Image sampling instructions.
         */
        if (in_func &&
            wc >= 5 &&
            (op == SpvOpImageSampleImplicitLod ||
             op == SpvOpImageSampleExplicitLod ||
             op == SpvOpImageSampleDrefImplicitLod ||
             op == SpvOpImageSampleDrefExplicitLod))
        {
            if (fs_find_load(s, w[i + 3]) >= 0)
            {
                STEREO_LOG(
                    "FS_PATCH_COUNTER sample image=%u result=%u coord=%u total=%u",
                    w[i + 3],
                    w[i + 2],
                    w[i + 4],
                    count + 1);
                ++count;
            }
        }
        /*
         * ImageFetch
         */
        if (in_func &&
            op == SpvOpImageFetch &&
            wc >= 5)
        {
            uint32_t descriptor_var = 0;
            int load =
                fs_find_load(
                    s,
                    w[i + 3]);
            if (load >= 0)
                descriptor_var =
                    s->loads[load].owner_var;
            if (descriptor_var == 0)
            {
                STEREO_LOG(
                    "FS_FETCH_NO_DESCRIPTOR image=%u",
                    w[i + 3]);
            }
            STEREO_LOG(
                "FS_FETCH_CLASSIFY image=%u descriptor=%u",
                w[i + 3],
                descriptor_var);
            if (fs_should_patch_sample(s, hash_spv(w, c), descriptor_var))
            {
                uint32_t binding = 0xffffffffu;
                int var =
                    fs_var_index(
                        s,
                        descriptor_var);
                if (var >= 0)
                    binding =
                        s->vars[var].binding;
                STEREO_LOG(
                    "FS_SAMPLE_PATCH_APPLY descriptor=%u binding=%u",
                    descriptor_var,
                    binding);
                STEREO_LOG(
                    "FS_PATCH_COUNTER fetch image=%u result=%u coord=%u total=%u",
                    w[i + 3],
                    w[i + 2],
                    w[i + 4],
                    count + 1);
                ++count;
            }
        }
        i += wc;
    }
    return count;
}

static int
fs_image_index(
    const FsScan *s,
    uint32_t id)
{
    if (!s)
        return -1;

    for (uint32_t i = 0; i < s->n_img; ++i)
    {
        if (s->images[i].id == id)
            return (int)i;
    }

    return -1;
}

static bool
fs_type_is_input_attachment(
    const FsScan *s,
    uint32_t type)
{
    if (!s)
        return false;

    for (uint32_t i = 0; i < s->n_var; ++i)
    {
        if (s->vars[i].type == type &&
            s->vars[i].storage ==
                SpvStorageClassInput)
        {
            return true;
        }
    }

    return false;
}

bool spirv_patch_stereo_fs(
    const uint32_t *in, size_t in_c,
    uint32_t **out, size_t *out_c)
{
    STEREO_LOG("CALLED spirv_patch_stereo_fs");
    if (!in || in_c < 5 || in[0] != SPIRV_MAGIC) return false;
    STEREO_LOG(
        "FS_PATCH_ENTER hash=%016llx words=%zu",
        (unsigned long long)hash_spv(in, in_c),
        in_c);
    uint64_t h = hash_spv(in, in_c);
    STEREO_LOG(
        "FS_PATCH_MODULE hash=%016llx words=%zu",
        (unsigned long long)h,
        in_c);
    FsScan s;
    fs_prescan(&s, in, in_c);
    for (uint32_t i = 0; i < s.n_img; ++i)
    {
        STEREO_LOG(
            "FS_IMAGE_TABLE "
            "type=%u "
            "sampledType=%u "
            "owner=%u "
            "set=%u "
            "binding=%u",
            s.images[i].id,
            s.images[i].sampled_type,
            s.images[i].owner_var,
            s.images[i].set,
            s.images[i].binding);
    }
    for (uint32_t i = 0; i < s.n_var; ++i)
    {
        const FsVariableInfo *var = &s.vars[i];
        if (var->binding != 0xffffffffu)
        {
            STEREO_LOG(
                "FS_DESCRIPTOR_SUMMARY var=%u set=%u binding=%u type=%u",
                var->id,
                var->set,
                var->binding,
                var->type);
        }
    }
    if (s.n_img == 0 || !s.float_id)
    {
        STEREO_LOG(
            "FS_PATCH_REJECT images=%u float_id=%u",
            s.n_img,
            s.float_id);
        return false;
    }
    uint32_t n_patches = fs_count_patches(&s, in, in_c);
    /* Allocate new IDs above current bound */
    uint32_t nid           = in[3];
    uint32_t new_int_id    = s.int_id        ? s.int_id        : nid++;
    uint32_t new_v3f_id    = s.v3float_id    ? s.v3float_id    : nid++;
    uint32_t new_v3i_id    = nid++;
    uint32_t new_pin_id    = s.ptr_int_in_id ? s.ptr_int_in_id : nid++;
    uint32_t new_vi_id     = s.vi_var_id     ? s.vi_var_id     : nid++;
    bool     is_new_vi     = (s.vi_var_id == 0);
    uint32_t samp_nid      = nid;
    uint32_t new_bound     = samp_nid + n_patches * 5 + 8;
    SpvBuf ob;
    if (!sb_init(&ob, in_c + 60 + (size_t)n_patches * 28)) return false;
    bool mv_added   = s.has_mv_cap;
    bool ext_done   = false;
    uint32_t spv_version = in[1];
    bool need_mv_ext =
        !s.has_mv_cap &&
        ((spv_version >> 16) == 1) &&
        (((spv_version >> 8) & 0xff) == 0);
    bool types_done = false;
    bool ep_done    = false;
    bool in_func    = false;
    /* Header */
    sb_push_n(&ob, in, 5);
    ob.w[3] = new_bound;
    for (size_t i = 5; i < in_c; ) {
        uint32_t op = in[i] & 0xffff, wc = in[i] >> 16;
        if (!wc || i + wc > in_c) break;
        if (in_func &&
            op >= SpvOpImageSampleImplicitLod &&
            op <= SpvOpImageSparseTexelsResident)
        {
            STEREO_LOG(
                "FS_IMAGE_OPCODE op=%u (%s) wc=%u resultType=%u result=%u",
                op,
                spv_op_name(op),
                wc,
                (wc >= 2) ? in[i + 1] : 0,
                (wc >= 3) ? in[i + 2] : 0);
        }
        /* Emit MultiView capability immediately before the first non-capability. */
        if (!mv_added &&
            op != SpvOpCapability)
        {
            uint32_t mv[] =
            {
                op_(SpvOpCapability, 2),
                SpvCapabilityMultiView
            };
            sb_push_n(&ob, mv, 2);
            mv_added = true;
        }
        /* SPIR-V 1.0 requires SPV_KHR_multiview immediately after capabilities. */
        if (!ext_done &&
            need_mv_ext &&
            op != SpvOpCapability)
        {
            uint32_t e[] =
            {
                op_(SpvOpExtension, 6),
                0x5F565053, /* SPV_ */
                0x5F52484B, /* KHR_ */
                0x746C756D, /* mult */
                0x65697669, /* ivie */
                0x00000077  /* w */
            };
            sb_push_n(&ob, e, 6);
            ext_done = true;
        }
        /* Modify OpEntryPoint: append new_vi_id to interface if we're adding it */
        if (op == 15 && !ep_done) {
            ep_done = true;
            if (is_new_vi) {
                sb_push(&ob, ((wc+1)<<16)|15);
                sb_push_n(&ob, &in[i+1], wc-1);
                sb_push(&ob, new_vi_id);
            } else {
                sb_push_n(&ob, &in[i], wc);
            }
            i += wc; continue;
        }
        /* Patch OpTypeImage: Dim=2D Arrayed=0 → Arrayed=1 (in-place word change) */
        if (op == SpvOpTypeImage && wc >= 9 &&
            in[i+3] == SpvDim2D &&
            in[i+5] == 0)
        {
            STEREO_LOG(
                "FS_IMAGE_PATCH_DETAIL type=%u sampled=%u dim=%u depth=%u arrayed=%u ms=%u format=%u",
                in[i+1],
                in[i+7],
                in[i+3],
                in[i+4],
                in[i+5],
                in[i+6],
                in[i+8]);
            sb_push_n(&ob, &in[i], wc);
            ob.w[ob.n - wc + 5] = 1;   /* Arrayed */
            STEREO_LOG(
                "FS_IMAGE_ARRAY_PATCH type=%u oldArrayed=%u newArrayed=%u ms=%u",
                in[i+1],
                in[i+5],
                ob.w[ob.n - wc + 5],
                in[i+6]);
            for (uint32_t img = 0; img < s.n_img; ++img)
            {
                if (s.images[img].id == in[i+1])
                {
                    STEREO_LOG(
                        "FS_IMAGE_PATCH_USER "
                        "type=%u "
                        "owner=%u "
                        "set=%u "
                        "binding=%u",
                        s.images[img].id,
                        s.images[img].owner_var,
                        s.images[img].set,
                        s.images[img].binding);
                    break;
                }
            }
            STEREO_LOG(
                "FS_PATCH_IMAGE word=%zu type=%u dim=%u depth=%u arrayed=%u",
                i,
                in[i+1],
                in[i+3],
                in[i+4],
                in[i+5]);
            STEREO_LOG(
                "FS discovered image type id=%u depth=%u arrayed=%u sampled=%u",
                in[i+1],
                in[i+4],
                in[i+5],
                in[i+7]);
            STEREO_LOG(
                "FS_ARRAY_UPGRADE type=%u",
                in[i+1]);
            STEREO_LOG(
                "FS converting image type id=%u depth=%u arrayed=%u",
                in[i+1],
                in[i+4],
                in[i+5]);
            i += wc;
            continue;
        }
        /* Inject new types + gl_ViewIndex variable before first OpFunction */
        if (op == 54 && !types_done) {
            types_done = true;
            in_func    = true;
            if (is_new_vi) {
                /* OpDecorate %vi BuiltIn ViewIndex */
                { uint32_t w[]={(4u<<16)|71, new_vi_id, 11, 4440};
                  sb_push_n(&ob,w,4); }
            }
            if (!s.int_id) {
                uint32_t w[]={(4u<<16)|21, new_int_id, 32, 1};
                sb_push_n(&ob,w,4); }
            if (!s.v3float_id) {
                uint32_t w[]={(4u<<16)|23, new_v3f_id, s.float_id, 3};
                sb_push_n(&ob,w,4); }
            {
                uint32_t w[]={(4u<<16)|23, new_v3i_id, new_int_id, 3};
                sb_push_n(&ob,w,4);
            }
            if (!s.ptr_int_in_id) {
                uint32_t w[]={(4u<<16)|32, new_pin_id, 1, new_int_id};
                sb_push_n(&ob,w,4); }
            if (is_new_vi) {
                /* OpVariable %ptr_int_in Input → %vi */
                uint32_t w[]={(4u<<16)|59, new_pin_id, new_vi_id, 1};
                sb_push_n(&ob,w,4); }
            sb_push_n(&ob, &in[i], wc);
            i += wc; continue;
        }
        if (op == 54) in_func = true;
        /*
         * Log fragment shader output stores.
         * OpStore operands:
         *   word[1] = target variable
         *   word[2] = stored value
         *
         * Used to identify SSAO/deferred lighting outputs.
         */
        if (in_func && op == 62 && wc >= 3)
        {
            uint32_t target = in[i+1];
            int vi = fs_var_index(&s, target);
            if (vi >= 0)
            {
                STEREO_LOG(
                    "FS_OUTPUT target=%u set=%u location=%u type=%u value=%u",
                    target,
                    s.vars[vi].set,
                    s.vars[vi].binding,
                    s.vars[vi].type,
                    in[i+2]);
            }
            else
            {
                STEREO_LOG(
                    "FS_OUTPUT_UNKNOWN target=%u value=%u",
                    target,
                    in[i+2]);
            }
        }
        /* Extend 2D sampling coordinate to 3D for patched loads */
        if (in_func && wc >= 5 &&
            (op == SpvOpImageSampleImplicitLod ||
             op == SpvOpImageSampleExplicitLod ||
             op == SpvOpImageSampleDrefImplicitLod ||
             op == SpvOpImageSampleDrefExplicitLod) &&
            fs_find_load(&s, in[i+3]) >= 0)
        {
            STEREO_LOG(
                "FS extending sample: op=%u sampledImage=%u coord=%u result=%u",
                op,
                in[i+3],
                in[i+4],
                in[i+2]);
            uint32_t coord_id = in[i+4];
            uint32_t descriptor_var = 0;
            int load =
                fs_find_load(
                    &s,
                    in[i+3]);
            if (load >= 0)
            {
                descriptor_var =
                    s.loads[load].owner_var;
                int vi =
                    fs_var_index(
                        &s,
                        descriptor_var);
                STEREO_LOG(
                    "FS_SAMPLE_DESCRIPTOR image=%u descriptorVar=%u set=%u binding=%u",
                    in[i+3],
                    descriptor_var,
                    (vi >= 0) ? s.vars[vi].set : 0xffffffffu,
                    (vi >= 0) ? s.vars[vi].binding : 0xffffffffu);
                if (vi >= 0)
                {
                    STEREO_LOG(
                        "FS_DESCRIPTOR_TYPE "
                        "descriptor=%u "
                        "type=%u "
                        "storage=%u "
                        "set=%u "
                        "binding=%u",
                        descriptor_var,
                        s.vars[vi].type,
                        s.vars[vi].storage,
                        s.vars[vi].set,
                        s.vars[vi].binding);
                }
                STEREO_LOG(
                    "FS_SAMPLE_BINDING_DETAIL image=%u descriptor=%u binding=%u",
                    in[i+3],
                    descriptor_var,
                    (vi >= 0) ? s.vars[vi].binding : 0xffffffffu);
                STEREO_LOG(
                    "FS_SAMPLE_MATCH image=%u load=%d var=%u",
                    in[i+3],
                    load,
                    descriptor_var);
            }
            if (!fs_should_patch_sample(&s, h, descriptor_var))
            {
                STEREO_LOG(
                    "FS_SAMPLE_SKIP_MONO image=%u descriptor=%u",
                    in[i+3],
                    descriptor_var);
            
                sb_push_n(&ob, &in[i], wc);
                i += wc;
                continue;
            }
            int vi = fs_var_index(&s, descriptor_var);
            STEREO_LOG(
                "FS_SAMPLE_PATCH_APPLY "
                "hash=%016llx "
                "image=%u "
                "descriptor=%u "
                "set=%u "
                "binding=%u",
                (unsigned long long)h,
                in[i+3],
                descriptor_var,
                (vi >= 0) ? s.vars[vi].set : 0xffffffffu,
                (vi >= 0) ? s.vars[vi].binding : 0xffffffffu);
            int image_type = -1;
            int sampled_type = -1;
            for (uint32_t v = 0; v < s.n_var; ++v)
            {
                if (s.vars[v].id == descriptor_var)
                {
                    sampled_type = s.vars[v].type;
                    break;
                }
            }
            STEREO_LOG(
                "FS_DESCRIPTOR_TYPES descriptor=%u sampledType=%d",
                descriptor_var,
                sampled_type);
            fs_dump_descriptor_chain(
                &s,
                in,
                in_c,
                descriptor_var);
            uint32_t id_lv  = samp_nid++;
            uint32_t id_cvt = samp_nid++;
            uint32_t id_u   = samp_nid++;
            uint32_t id_v   = samp_nid++;
            uint32_t id_c3  = samp_nid++;
            /* OpLoad %int %vi → id_lv */
            { uint32_t w[]={(4u<<16)|61, new_int_id, id_lv, new_vi_id};
              sb_push_n(&ob,w,4); }
            /* OpConvertSToF %float id_lv → id_cvt */
            { uint32_t w[]={(4u<<16)|111, s.float_id, id_cvt, id_lv};
              sb_push_n(&ob,w,4); }
            /* OpCompositeExtract %float coord 0 → id_u */
            { uint32_t w[]={(5u<<16)|81, s.float_id, id_u, coord_id, 0};
              sb_push_n(&ob,w,5); }
            /* OpCompositeExtract %float coord 1 → id_v */
            { uint32_t w[]={(5u<<16)|81, s.float_id, id_v, coord_id, 1};
              sb_push_n(&ob,w,5); }
            /* OpCompositeConstruct %v3float id_u id_v id_cvt → id_c3 */
            { uint32_t w[]={(6u<<16)|80, new_v3f_id, id_c3, id_u, id_v, id_cvt};
              sb_push_n(&ob,w,6); }
            /* Emit modified sample instruction: word[4] = new coord */
            STEREO_LOG(
                "FS_SAMPLE_REWRITE "
                "result=%u "
                "sampledImage=%u "
                "descriptor=%u "
                "coord2d=%u "
                "coord3d=%u "
                "opcode=%s",
                in[i + 2],
                in[i + 3],
                descriptor_var,
                coord_id,
                id_c3,
                spv_op_name(op));
            sb_push(&ob, in[i]);          /* opcode */
            sb_push(&ob, in[i+1]);        /* result type */
            sb_push(&ob, in[i+2]);        /* result id */
            sb_push(&ob, in[i+3]);        /* sampled image (unchanged) */
            sb_push(&ob, id_c3);          /* new 3D coordinate */
            if (wc > 5) sb_push_n(&ob, &in[i+5], wc-5); /* image operands */
            STEREO_LOG(
                "FS_SAMPLE_WRITTEN "
                "result=%u "
                "coord=%u",
                in[i + 2],
                id_c3);
            i += wc; continue;
        }
        if (in_func && op == 86 && wc >= 3)
        {
            STEREO_LOG(
                "FS_IMAGE imageResult=%u sampledImage=%u",
                in[i+2],
                in[i+3]);
        }
        /* Extend OpImageFetch ivec2 -> ivec3(x,y,ViewIndex) */
        if (in_func && op == 95 && wc >= 5)
        {
            //STEREO_LOG(
            //    "FS_FETCH opcode image=%u coord=%u result=%u",
            //    in[i+3],
            //    in[i+4],
            //    in[i+2]);
            //STEREO_LOG(
            //    "FS_FETCH_PATCH_ENTER image=%u result=%u",
            //    in[i+3],
            //    in[i+2]);
            uint32_t coord_id = in[i+4];
            uint32_t descriptor_var = 0;
            bool image_known = false;
            int load =
                fs_find_load(
                    &s,
                    in[i+3]);
            if (load >= 0)
            {
                descriptor_var =
                    s.loads[load].owner_var;
                image_known = true;
                //STEREO_LOG(
                //    "FS_FETCH_MATCH image=%u loadIndex=%d load=%u var=%u",
                //    in[i+3],
                //    load,
                //    s.loads[load].id,
                //    descriptor_var);
            }
            //STEREO_LOG(
            //    "FS_FETCH_PATCH_DECISION image=%u known=%u descriptor=%u",
            //    in[i+3],
            //    image_known,
            //    descriptor_var);
            //if (load >= 0)
            //{
            //    STEREO_LOG(
            //        "FS_FETCH_FOUND image=%u loadIndex=%d var=%u",
            //        in[i+3],
            //        load,
            //        s.loads[load].owner_var);
            //}
            //else
            //{
            //    STEREO_LOG(
            //        "FS_FETCH_UNKNOWN image=%u",
            //        in[i+3]);
            //}
            int vi = fs_var_index(&s, descriptor_var);
            STEREO_LOG(
                "FS_SAMPLE_PATCH_APPLY hash=%016llx image=%u descriptor=%u set=%u binding=%u",
                (unsigned long long)h,
                in[i+3],
                descriptor_var,
                (vi >= 0) ? s.vars[vi].set : 0xffffffffu,
                (vi >= 0) ? s.vars[vi].binding : 0xffffffffu);
            if (vi >= 0)
            {
                STEREO_LOG(
                    "FS_FETCH_VAR_INFO image=%u var=%u storage=%u type=%u set=%u binding=%u",
                    in[i+3],
                    descriptor_var,
                    s.vars[vi].storage,
                    s.vars[vi].type,
                    s.vars[vi].set,
                    s.vars[vi].binding);
            }
            STEREO_LOG(
                "FS_FETCH_DESCRIPTOR image=%u descriptorVar=%u set=%u binding=%u",
                in[i+3],
                descriptor_var,
                (vi >= 0) ? s.vars[vi].set : 0xffffffffu,
                (vi >= 0) ? s.vars[vi].binding : 0xffffffffu);
            if (in[i+3] == 47)
            {
                STEREO_LOG(
                    "FS_TRACE_IMAGE47 result=%u coord=%u",
                    in[i+2],
                    coord_id);
            }
            if (!fs_binding_is_stereo_attachment(&s, descriptor_var))
            {
                STEREO_LOG(
                    "FS_FETCH_SKIP_MONO image=%u descriptor=%u binding_not_stereo",
                    in[i+3],
                    descriptor_var);
                sb_push_n(&ob, &in[i], wc);
                i += wc;
                continue;
            }
            STEREO_LOG(
                "FS_FETCH_PATCH hash=%016llx image=%u descriptor=%u set=%u binding=%u",
                (unsigned long long)h,
                in[i+3],
                descriptor_var,
                (vi >= 0) ? s.vars[vi].set : 0xffffffffu,
                (vi >= 0) ? s.vars[vi].binding : 0xffffffffu);
            STEREO_LOG(
                "FS_FETCH_OPCODE opcode=%u image=%u coord=%u result=%u",
                op,
                in[i+3],
                in[i+4],
                in[i+2]);
            STEREO_LOG(
                "FS_FETCH_STEREO_PATCH image=%u descriptorVar=%u coord=%u",
                in[i+3],
                descriptor_var,
                in[i+4]);
            fs_dump_descriptor_chain(
                &s,
                in,
                in_c,
                descriptor_var);
            uint32_t id_lv = samp_nid++;
            uint32_t id_x  = samp_nid++;
            uint32_t id_y  = samp_nid++;
            uint32_t id_c3 = samp_nid++;
            { uint32_t w[]={(4u<<16)|61, new_int_id, id_lv, new_vi_id};
              sb_push_n(&ob,w,4); }
            { uint32_t w[]={(5u<<16)|81, new_int_id, id_x, coord_id, 0};
              sb_push_n(&ob,w,5); }
            { uint32_t w[]={(5u<<16)|81, new_int_id, id_y, coord_id, 1};
              sb_push_n(&ob,w,5); }
            { uint32_t w[]={(6u<<16)|80, new_v3i_id, id_c3,
                            id_x, id_y, id_lv};
              sb_push_n(&ob,w,6); }
            sb_push(&ob, in[i]);
            sb_push(&ob, in[i+1]);
            sb_push(&ob, in[i+2]);
            sb_push(&ob, in[i+3]);
            sb_push(&ob, id_c3);
            if (wc > 5)
                sb_push_n(&ob, &in[i+5], wc - 5);
            STEREO_LOG(
                "FS_FETCH_PATCH_DONE image=%u newCoord=%u",
                in[i+3],
                id_c3);
            i += wc;
            continue;
        }
        sb_push_n(&ob, &in[i], wc);
        i += wc;
    }
    for (size_t j = 5; j < ob.n; )
    {
        uint32_t wc = ob.w[j] >> 16;
        uint32_t op = ob.w[j] & 0xffff;
        if (!wc || j + wc > ob.n)
            break;
        if (op == SpvOpTypeImage && wc >= 9)
        {
            STEREO_LOG(
                "FS_OUTPUT_IMAGE_TYPE id=%u sampledType=%u dim=%u depth=%u arrayed=%u ms=%u sampled=%u format=%u",
                ob.w[j + 1],
                ob.w[j + 2],
                ob.w[j + 3],
                ob.w[j + 4],
                ob.w[j + 5],
                ob.w[j + 6],
                ob.w[j + 7],
                ob.w[j + 8]);
        }
        j += wc;
    }
    ob.w[3] = samp_nid + 1;
    *out   = ob.w;
    *out_c = ob.n;
    STEREO_LOG("FS patched: %u 2D img types→arr, %u samples extended, bound %u→%u",
               s.n_img, n_patches, in[3], ob.w[3]);
    STEREO_LOG(
        "FS_PATCH_DONE hash=%016llx words=%zu new_words=%zu",
        (unsigned long long)hash_spv(in, in_c),
        in_c,
        ob.n);
    return true;
}

/* ── Helpers ─────────────────────────────────────────────────────────────── */
static bool is_patchable_spv(const uint32_t *w, size_t c)
{
    if (c<5||w[0]!=SPIRV_MAGIC) return false;
    for (size_t i=5;i<c;) {
        uint32_t op=w[i]&0xffff, wc=w[i]>>16; if (!wc||i+wc>c) break;
        if (op==SpvOpEntryPoint&&wc>=2) {
            uint32_t e=w[i+1];
            return e==SpvExecVertex||e==SpvExecGeometry||e==SpvExecTessEval||e==4/*Fragment*/;
        }
        i+=wc;
    }
    return false;
}

static StereoShaderCache *
cache_find(StereoDevice *sd, VkShaderModule mod)
{
    if (!sd)
        return NULL;
    for (uint32_t i = 0; i < sd->shader_cache_count; i++)
    {
        StereoShaderCache *e = &sd->shader_cache[i];
        if (e->handle == mod)
        {
            STEREO_LOG(
                "CACHE_FIND_HIT module=%p hash=%016llx words=%zu",
                (void *)mod,
                (unsigned long long)hash_spv(e->spv, e->words),
                e->words);
            return e;
        }
    }
    STEREO_LOG(
        "CACHE_FIND_MISS module=%p",
        (void *)mod);
    return NULL;
}

static void cache_add(StereoDevice *sd, VkShaderModule h,
                      const uint32_t *spv, size_t words) {
    if (sd->shader_cache_count>=MAX_SHADER_CACHE) return;
    uint32_t *cp=malloc(words*4); if (!cp) return;
    memcpy(cp,spv,words*4);
    CHECK_ARRAY_COUNT(sd->shader_cache_count, MAX_SHADER_CACHE, "shader_cache_count");
    StereoShaderCache *e=&sd->shader_cache[sd->shader_cache_count++];
    e->handle=h; e->spv=cp; e->words=words;
}
static void cache_remove(StereoDevice *sd, VkShaderModule h)
{
    for (uint32_t i = 0; i < sd->shader_cache_count; i++)
    {
        if (sd->shader_cache[i].handle == h)
        {
            free(sd->shader_cache[i].spv);
            uint32_t last = --sd->shader_cache_count;
            if (i != last)
                sd->shader_cache[i] = sd->shader_cache[last];
            memset(&sd->shader_cache[last], 0,
                   sizeof(sd->shader_cache[last]));
            return;
        }
    }
}

/* ── vkCreateShaderModule ─────────────────────────────────────────────────── */
VKAPI_ATTR VkResult VKAPI_CALL
stereo_CreateShaderModule(VkDevice device, const VkShaderModuleCreateInfo *pCI,
                          const VkAllocationCallbacks *pAlloc, VkShaderModule *pSM)
{
    STEREO_LOG("CALLED stereo_CreateShaderModule");
    StereoDevice *sd=stereo_device_from_handle(device);
    if (!sd) return VK_ERROR_DEVICE_LOST;
    STEREO_LOG("CALL real CreateShaderModule");
    VkResult res=sd->real.CreateShaderModule(sd->real_device,pCI,pAlloc,pSM);
    STEREO_LOG("RETURN real CreateShaderModule result=%d", res);
    if (res!=VK_SUCCESS) return res;
    if (!sd->stereo.enabled) return VK_SUCCESS;
    const uint32_t *spv = (const uint32_t *)pCI->pCode;
    size_t wc = pCI->codeSize / 4;
    uint64_t h = hash_spv(spv, wc);
    STEREO_LOG(
        "CREATE_SHADER module=%p hash=%016llx words=%zu patchable=%u",
        (void *)*pSM,
        (unsigned long long)h,
        wc,
        is_patchable_spv(spv, wc));
    STEREO_LOG(
        "SHADER_MODULE words=%u hash=%016llx",
        wc,
        (unsigned long long)h);
    const char *dump = stereo_getenv("VKS3D_DUMP_SPIRV");
    if (dump)
    {
        char dp[512];
        _snprintf(
            dp,
            sizeof(dp) - 1,
            "%s\\%016llx.spv",
            dump,
            (unsigned long long)h);
        FILE *f = fopen(dp, "rb");
        if (!f)
        {
            f = fopen(dp, "wb");
            if (f)
            {
                fwrite(
                    spv,
                    4,
                    wc,
                    f);
                fclose(f);
            }
        }
        else
        {
            fclose(f);
        }
    }
    if (is_patchable_spv(spv, wc))
    {
        cache_add(sd, *pSM, spv, wc);
    }
    STEREO_LOG(
        "CREATE_SHADER_DONE module=%p hash=%016llx",
        (void *)*pSM,
        (unsigned long long)h);
    return VK_SUCCESS;
}

/* ── vkCreateGraphicsPipelines ───────────────────────────────────────────── */
VKAPI_ATTR VkResult VKAPI_CALL
stereo_CreateGraphicsPipelines(VkDevice device, VkPipelineCache pc,
    uint32_t N, const VkGraphicsPipelineCreateInfo *pCI,
    const VkAllocationCallbacks *pAlloc, VkPipeline *pP)
{
    STEREO_LOG(
        "CALLED stereo_CreateGraphicsPipelines this=%p",
        (void*)&stereo_CreateGraphicsPipelines);
    StereoDevice *sd=stereo_device_from_handle(device);
    if (!sd) return VK_ERROR_DEVICE_LOST;
    STEREO_LOG("PIPE_IN_RAW N=%u pCI=%p first=%p renderPass=%p stageCount=%u pNext=%p",
               N,
               (void*)pCI,
               (N > 0 ? (void*)pCI[0].renderPass : NULL),
               (N > 0 ? (void*)pCI[0].renderPass : NULL),
               (N > 0 ? pCI[0].stageCount : 0),
               (N > 0 ? pCI[0].pNext : NULL));
    STEREO_LOG(
        "PIPE_CREATE_BEGIN N=%u multiview=%d enabled=%d",
        N,
        sd->stereo.multiview,
        sd->stereo.enabled);
    if (!sd->stereo.enabled)
        return sd->real.CreateGraphicsPipelines(sd->real_device,pc,N,pCI,pAlloc,pP);
    VkShaderModule                   *tmp_mod = calloc(N, sizeof(VkShaderModule));
    VkPipelineShaderStageCreateInfo **tst     = calloc(N, sizeof(void*));
    VkGraphicsPipelineCreateInfo     *infos   = malloc(N * sizeof(*infos));
    StereoDebugCtx                   *dbg_out = calloc(N, sizeof(*dbg_out));
    for (uint32_t i = 0; i < N; i++)
    {
        dbg_out[i].proj_set             = UINT32_MAX;
        dbg_out[i].proj_binding         = UINT32_MAX;
        dbg_out[i].proj_member_mask     = UINT32_MAX;
        dbg_out[i].proj_var             = UINT32_MAX;
    }
    if (!tmp_mod||!tst||!infos) {
        free(tmp_mod); free(tst); free(infos);
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    memcpy(infos, pCI, N * sizeof(*infos));
    for (uint32_t p = 0; p < N; p++) {
        STEREO_LOG(
            "PIPE_IN p=%u rp=%p stageCount=%u vs=%d tcs=%d tes=%d pNext=%p",
            p,
            (void*)pCI[p].renderPass,
            pCI[p].stageCount,
            (pCI[p].pVertexInputState != NULL),
            0,
            0,
            pCI[p].pNext);
    }
    const char *dump = stereo_getenv("VKS3D_DUMP_SPIRV");
    static int  dump_n = 0;
    float lo=sd->stereo.left_eye_offset, ro=sd->stereo.right_eye_offset,
          conv=sd->stereo.convergence;
    STEREO_LOG(
        "[PATCH] lo=%f ro=%f flip=%d",
        lo,
        ro,
        sd->stereo.flip_eyes);
    for (uint32_t p=0; p<N; p++) {
        const VkGraphicsPipelineCreateInfo *ci=&pCI[p];
        //REMOVED StereoPipelineInfo *info =
        //REMOVED     add_pipeline_info(sd);
        const VkBaseInStructure *base =
            (const VkBaseInStructure*)ci->pNext;
        uint32_t view_mask = 0;
        /* ── Safety: Vulkan 1.3 dynamic rendering pipelines may not use pNext ── */
        while (base)
        {
            if (base->sType ==
                VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO)
            {
                const VkPipelineRenderingCreateInfo *ri =
                 (const VkPipelineRenderingCreateInfo*)base;
                VkPipelineRenderingCreateInfo *rw =
                 (VkPipelineRenderingCreateInfo*)base;
                
                /* Dynamic rendering path: if stereo is enabled and the app left
                 * viewMask at 0, promote it to 0x3 so the pipeline is actually
                 * created for multiview. */
                if (sd->stereo.multiview && rw->viewMask == 0) {
                 STEREO_LOG(
                  "PIPE_RENDERING_UPGRADE p=%u viewMask 0x0->0x3 colors=%u depth=%u stencil=%u",
                  p,
                  ri->colorAttachmentCount,
                  ri->depthAttachmentFormat,
                  ri->stencilAttachmentFormat);
                 rw->viewMask = 0x3;
                }
                view_mask = rw->viewMask;
                STEREO_LOG(
                    "PIPE_RENDERING_CAPTURE p=%u viewMask=0x%x colors=%u depth=%u stencil=%u",
                    p,
                    rw->viewMask,
                    ri->colorAttachmentCount,
                    ri->depthAttachmentFormat,
                    ri->stencilAttachmentFormat);
            }
            base = base->pNext;
        }
        if (!ci || ci->stageCount == 0 || !ci->pStages) {
            STEREO_LOG(
                "PIPE_EMPTY_STAGE_PIPELINE p=%u rp=%p pNext=%p stageCount=%u pStages=%p isUI=%d isComputeLike=%d",
                p,
                ci ? (void*)ci->renderPass : NULL,
                ci ? (void*)ci->pNext : NULL,
                ci ? ci->stageCount : 0,
                ci ? (void*)ci->pStages : NULL,
                (ci && ci->pVertexInputState == NULL),
                (ci && ci->stageCount == 0));
        }
        if (!ci ||
            ci->stageCount == 0 ||
            !ci->pStages)
        {
            STEREO_LOG("PIPE_INVALID p=%u ci=%p stageCount=%u pStages=%p renderPass=%p",
                       p,
                       (void*)ci,
                       ci ? ci->stageCount : 0,
                       ci ? (void*)ci->pStages : NULL,
                       ci ? (void*)ci->renderPass : NULL);
            continue;
        }
        bool has_vs=false, has_tcs=false, has_tes=false;
        uint32_t vs_stage=~0u, tes_stage=~0u;
        for (uint32_t s=0;s<ci->stageCount;s++) {
            VkShaderStageFlagBits st=ci->pStages[s].stage;
            if (st==VK_SHADER_STAGE_VERTEX_BIT)
                { has_vs=true; vs_stage=s; }
            if (st==VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT)
                has_tcs=true;
            if (st==VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT)
                { has_tes=true; tes_stage=s; }
        }
        /* ── Determine if this pipeline's render pass has multiview ──────
         * gl_ViewIndex is 0 in non-multiview passes.  Patching VS/TES there
         * bakes in left_eye_offset for ALL draws → deferred G-buffer / shadow
         * passes render from left-eye-only perspective → monoscopic output.
         * Leave non-multiview pass shaders unpatched so G-buffer, shadow maps,
         * and post-fx all render from the CENTER perspective; the multiview
         * final (swapchain) pass applies per-eye shift → image-space stereo
         * for deferred content with shadows/lights/bloom properly aligned.   */
        StereoRenderPassInfo *rpi = NULL;
        bool in_mv_rp = false;
        if (ci->renderPass != VK_NULL_HANDLE) {
            rpi = stereo_rp_lookup(sd, ci->renderPass);
            /* Render-pass pipelines are multiview only if the render pass itself
             * was created with multiview support. Shadow passes must stay mono. */
            in_mv_rp =
                (rpi && rpi->has_multiview) ||
                ((view_mask & 0x3) != 0);
        }
        else if (sd->stereo.multiview && (view_mask & 0x3) != 0) {
        /* VK 1.3 dynamic rendering: no renderPass handle, but we already
         * upgraded VkPipelineRenderingCreateInfo.viewMask above. Treat it
         * as multiview so VS/TES patching still runs. */
        in_mv_rp = true;
        }
        STEREO_LOG(
            "PIPE_DECISION p=%u rp=%p rpi=%p in_mv=%u view_mask=0x%x stages=%u has_vs=%u has_tes=%u quad=%u",
            p,
            (void*)ci->renderPass,
            (void*)rpi,
            (unsigned)in_mv_rp,
            view_mask,
            (unsigned)ci->stageCount,
            (unsigned)has_vs,
            (unsigned)has_tes,
            (!ci->pVertexInputState ||
             ci->pVertexInputState->vertexBindingDescriptionCount == 0));
        for (uint32_t fs_dbg_i = 0; fs_dbg_i < ci->stageCount; fs_dbg_i++) {
            if (ci->pStages[fs_dbg_i].stage == VK_SHADER_STAGE_FRAGMENT_BIT) {
                StereoShaderCache *fs_dbg =
                    cache_find(sd, ci->pStages[fs_dbg_i].module);
                if (fs_dbg) {
                    STEREO_LOG(
                        "ALL_FS_SHADER p=%u hash=%016llx words=%zu module=%p quad=%u in_mv=%u",
                        p,
                        (unsigned long long)hash_spv(fs_dbg->spv, fs_dbg->words),
                        fs_dbg->words,
                        (void*)ci->pStages[fs_dbg_i].module,
                        (!ci->pVertexInputState ||
                         ci->pVertexInputState->vertexBindingDescriptionCount == 0),
                        (unsigned)in_mv_rp);
                }
            }
        }
        /* ── PATCH 3: Pipeline multiview FIXED (NO pipeline struct exists) ─────────────── */
        /* Multiview is render-pass driven ONLY.
         * Pipeline pNext must NOT contain VkPipelineMultiviewCreateInfo (invalid Vulkan API). */
        if (in_mv_rp) {
            if (rpi && rpi->mv_handle) {
                STEREO_LOG(
                    "Pipe %u: MV RP detected (stageCount=%u) - using MV render pass %p",
                    p,
                    ci->stageCount,
                    (void*)rpi->mv_handle);
                /* render-pass pipeline path only */
                infos[p].renderPass = rpi->mv_handle;
            } else {
                STEREO_LOG(
                    "Pipe %u: dynamic rendering multiview detected (stageCount=%u) - no renderPass swap",
                    p,
                    ci->stageCount);
                /* VK 1.3 dynamic rendering: keep infos[p].renderPass as-is */
            }
        }
        if (!in_mv_rp)
        {
            STEREO_LOG(
                "Pipe %u: rp=%p not multiview (VS=%d TES=%d stages=%u)",
                p,
                (void*)(uintptr_t)ci->renderPass,
                has_vs,
                has_tes,
                ci->stageCount);
            /* IMPORTANT:
             * Do NOT patch renderpass-based multiview logic for clearly mono pipelines
             * BUT still allow FS quad / UI heuristics to run later
             */
            goto PIPE_DECISION_CONTINUE;
        }
        /* Substitute multiview render pass for pipeline compilation.
         * Pipelines must be compiled against the MV render pass so the driver
         * enables multiview optimisation and gl_ViewIndex receives the real
         * per-view index (0 or 1).  Render-pass compatibility rules allow these
         * pipelines to be used with both MV and non-MV framebuffers since
         * viewMask is not part of the compatibility criteria. */
        if (rpi && rpi->mv_handle && rpi->has_multiview && in_mv_rp)
        {
            /* Render-pass path only; dynamic rendering has no renderPass to swap. */
            infos[p].renderPass = rpi->mv_handle;
        }
        /* ── Full-screen quad detection ──────────────────────────────────
         * Pipelines with no vertex input bindings are full-screen quads used
         * by deferred lighting, SSAO, bloom, TAA, etc.  Their FS samples from
         * G-buffer / render-target textures (all upgraded to 2D_ARRAY by
         * stereo_CreateImage).  We patch the FS to use sampler2DArray +
         * gl_ViewIndex so each eye reads its own G-buffer layer.
         * The VS of a quad must NOT be patched — shifting the quad position
         * would prevent it covering the full screen for one eye.
         * Geometry pipelines (has vertex input) use Path A/B VS patching. */
        bool is_quad = !ci->pVertexInputState ||
                       ci->pVertexInputState->vertexBindingDescriptionCount == 0;
        STEREO_LOG(
            "FS_GATE p=%u quad=%u stageCount=%u",
            p,
            is_quad,
            ci->stageCount);
        if (is_quad && ci->stageCount > 0)
        {
            /* Find FS stage */
            uint32_t fs_s = ~0u;
            for (uint32_t s2 = 0; s2 < ci->stageCount; s2++)
            {
                if (ci->pStages[s2].stage == VK_SHADER_STAGE_FRAGMENT_BIT)
                {
                    fs_s = s2;
                    break;
                }
            }
            if (fs_s == ~0u)
            {
                STEREO_LOG("Pipe %u: quad but no FS stage", p);
                continue;
            }
            StereoShaderCache *fs_cache =
                cache_find(sd, ci->pStages[fs_s].module);
            if (!fs_cache)
            {
                STEREO_LOG(
                    "PIPE_MODULE_MISS stage=0x%x module=%p renderPass=%p pipeline=%u",
                    ci->pStages[fs_s].stage,
                    (void *)ci->pStages[fs_s].module,
                    (void *)ci->renderPass,
                    p);
                for (uint32_t k = 0; k < sd->shader_cache_count; ++k)
                {
                    STEREO_LOG(
                        "CACHE_HANDLE[%u] module=%p hash=%016llx words=%zu",
                        k,
                        (void *)sd->shader_cache[k].handle,
                        (unsigned long long)hash_spv(
                            sd->shader_cache[k].spv,
                            sd->shader_cache[k].words),
                        sd->shader_cache[k].words);
                }
                continue;
            }
            uint64_t spv_hash =
                hash_spv(fs_cache->spv, fs_cache->words);
            uint32_t pipeline_has_mv =
                (rpi != NULL) ? (uint32_t)rpi->has_multiview : 0;
            STEREO_LOG(
                "FS_PATCH_DECISION "
                "pipe=%u "
                "rp=%p "
                "subpass=%u "
                "is_quad=%u "
                "pipeline_mv=%u "
                "has_fs=%u "
                "fs_hash=%016llx "
                "cache=%p "
                "stageFlags=0x%x",
                p,
                (void *)ci->renderPass,
                ci->subpass,
                is_quad,
                pipeline_has_mv,
                (fs_s != ~0u),
                (unsigned long long)spv_hash,
                (void *)fs_cache,
                ci->pStages[fs_s].stage);
            STEREO_LOG(
                "QUAD_FS_SHADER p=%u hash=%016llx words=%zu module=%p",
                p,
                (unsigned long long)spv_hash,
                fs_cache->words,
                (void *)ci->pStages[fs_s].module);
            STEREO_LOG(
                "SHADER_MODULE stage=FS hash=%016llx words=%zu module=%p",
                (unsigned long long)spv_hash,
                fs_cache->words,
                (void *)ci->pStages[fs_s].module);
            STEREO_LOG(
                "PATCH hash=%016llx words=%zu module=%p vs_stage=%u",
                (unsigned long long)spv_hash,
                fs_cache->words,
                (void *)(has_vs ? ci->pStages[vs_stage].module : VK_NULL_HANDLE),
                vs_stage);
            if (dump)
            {
                char dp[512];

                _snprintf(
                    dp,
                    sizeof(dp) - 1,
                    "%s\\%016llx-fs.spv",
                    dump,
                    (unsigned long long)spv_hash);

                FILE *f = fopen(dp, "rb");
                if (!f)
                {
                    f = fopen(dp, "wb");
                    if (f)
                    {
                        fwrite(
                            fs_cache->spv,
                            4,
                            fs_cache->words,
                            f);
                        fclose(f);
                    }
                }
                else
                {
                    fclose(f);
                }
            }
            uint32_t *patched = NULL; size_t pc2 = 0;
            STEREO_LOG(
                "FS_PATCH_BEGIN hash=%016llx",
                (unsigned long long)spv_hash);
            STEREO_LOG(
                "PATCHING_FS hash=%016llx",
                (unsigned long long)spv_hash);
            STEREO_LOG(
                "CALLING spirv_patch_stereo_fs hash=%016llx words=%zu",
                (unsigned long long)spv_hash,
                fs_cache->words);
            /*
             * Analyze FS projection UBO usage.
             *
             * SSAO/reconstruction shaders often use the projection matrix
             * only in the fragment stage, so VS metadata is insufficient.
             */
            {
                SpvMod fm = {0};
                fm.words = fs_cache->spv;
                fm.count = fs_cache->words;
                fm.bound = fm.words[3];
                fm.value_capacity = fm.bound + 64;
                fm.value_from_matrix =
                    calloc(fm.value_capacity, sizeof(uint8_t));
                fm.is_matrix_type =
                    calloc(fm.value_capacity, sizeof(uint8_t));
                fm.is_matrix_ptr =
                    calloc(fm.value_capacity, sizeof(uint8_t));
                fm.is_proj_value =
                    calloc(fm.value_capacity, sizeof(uint8_t));
                fm.is_view_value =
                    calloc(fm.value_capacity, sizeof(uint8_t));
                if (fm.value_from_matrix &&
                    fm.is_matrix_type &&
                    fm.is_matrix_ptr &&
                    fm.is_proj_value &&
                    fm.is_view_value)
                {
                    spv_scan(&fm);
                    if (fm.proj_found)
                    {
                        dbg_out[p].has_proj_ubo = true;
                        dbg_out[p].proj_set = fm.proj_set;
                        dbg_out[p].proj_binding = fm.proj_binding;
                        dbg_out[p].proj_member_mask =
                            fm.proj_member_mask;
                        dbg_out[p].proj_var = fm.proj_var;
                        STEREO_LOG(
                            "FS_PROJ_FOUND hash=%016llx set=%u binding=%u mask=0x%X var=%u",
                            (unsigned long long)hash_spv(
                                fs_cache->spv,
                                fs_cache->words),
                            fm.proj_set,
                            fm.proj_binding,
                            fm.proj_member_mask,
                            fm.proj_var);
                    }
                }
                free_spv_provenance(&fm);
            }
            STEREO_LOG(
                "FS_PATCH_BEGIN hash=%016llx pipe=%u",
                (unsigned long long)spv_hash,
                p);
            if (!spirv_patch_stereo_fs(
                    fs_cache->spv,
                    fs_cache->words,
                    &patched,
                    &pc2))
            {
                STEREO_LOG(
                    "Pipe %u: FS patch skipped (no 2D samplers — material-only?)",
                    p);
                continue;
            }
            STEREO_LOG(
                "FS_PATCH_DONE hash=%016llx",
                (unsigned long long)spv_hash);
            STEREO_LOG(
                "spirv_patch_stereo_fs returned=%u patchedWords=%zu",
                patched ? 1 : 0,
                pc2);
            STEREO_LOG(
                "FS_PATCH_RETURN ptr=%p words=%zu hash=%016llx",
                (void *)patched,
                pc2,
                (unsigned long long)hash_spv(
                    patched,
                    pc2));
            STEREO_LOG(
                "FS_DUMP words=%zu ptr=%p hash=%016llx",
                pc2,
                (void *)patched,
                (unsigned long long)hash_spv(
                    patched,
                    pc2));
            if (dump) {
                char dp[512];
                _snprintf(
                    dp,
                    sizeof(dp)-1,
                    "%s\\%016llx+fs.spv",
                    dump,
                    (unsigned long long)spv_hash);
                FILE *f=fopen(dp,"wb");
                if (f) {
                    fwrite(patched,4,pc2,f);
                    fclose(f);
                }
            }
            VkShaderModuleCreateInfo smci={VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                NULL,0,pc2*4,patched};
            VkShaderModule tmp=VK_NULL_HANDLE;
            VkResult mr=sd->real.CreateShaderModule(sd->real_device,&smci,NULL,&tmp);
            spirv_patched_free(patched);
            if (mr!=VK_SUCCESS) {
                STEREO_ERR("Pipe %u: quad FS module err %d",p,mr); continue; }
            uint32_t sc2=ci->stageCount;
            VkPipelineShaderStageCreateInfo *st=malloc(sc2*sizeof(*st));
            if (!st) { sd->real.DestroyShaderModule(sd->real_device,tmp,NULL); continue; }
            memcpy(st,ci->pStages,sc2*sizeof(*st));
            st[fs_s].module = tmp;
            infos[p].pStages = st;
            tmp_mod[p] = tmp;
            tst[p] = st;
            STEREO_LOG(
                "PATCHED_STAGE PathFS p=%u stage=%u orig=%p patched=%p",
                p,
                fs_s,
                (void *)ci->pStages[fs_s].module,
                (void *)tmp);
            STEREO_LOG(
                "Pipe %u: Path FS — quad sampler2DArray patch (%u stages)",
                p,
                sc2);
            continue;
        }
        /* ── Path A: patch existing TES ──────────────────────────────── */
        if (has_tes && tes_stage!=~0u) {
            StereoShaderCache *e=cache_find(sd, ci->pStages[tes_stage].module);
            if (!e) { STEREO_LOG("Pipe %u PathA: TES not cached",p); continue; }
            STEREO_LOG(
                "SHADER_MODULE stage=TES hash=%016llx words=%zu module=%p",
                (unsigned long long)hash_spv(e->spv, e->words),
                e->words,
                (void*)ci->pStages[tes_stage].module);
            if (dump)
            {
                uint64_t spv_hash=hash_spv(e->spv, e->words);
                char dp[512];
                _snprintf(
                    dp,
                    sizeof(dp)-1,
                    "%s\\%016llx-ts.spv",
                    dump,
                    (unsigned long long)spv_hash);
                FILE *f = fopen(dp, "rb");
                if (!f)
                {
                    f = fopen(dp, "wb");
                    if (f)
                    {
                        fwrite(
                            e->spv,
                            4,
                            e->words,
                            f);
                        fclose(f);
                    }
                }
                else
                {
                    fclose(f);
                }
            }
            uint32_t *patched=NULL; size_t pc2=0;
            STEREO_LOG(
                "[CALL A] lo=%f ro=%f conv=%f flip=%d",
                lo,
                ro,
                conv,
                sd->stereo.flip_eyes);
                StereoDebugCtx dbgA = {
                    p,
                    ci->renderPass,
                    in_mv_rp,
                    (uint32_t)VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT,
                    0,
                    0
                };
                if (!spirv_patch_stereo_vertex(
                        &sd->stereo,
                        e->spv, e->words,
                        &patched, &pc2,
                        lo, ro, conv,
                        true,
                        &dbgA))
                {
                STEREO_LOG("TES patch failed");
                if (dump && patched && pc2) {
                    uint64_t spv_hash = hash_spv(e->spv, e->words);
                    char dp[512];
                    _snprintf(
                        dp,
                        sizeof(dp)-1,
                        "%s\\%016llx-ts_failed.spv",
                        dump,
                        (unsigned long long)spv_hash);
                    FILE *f=fopen(dp,"wb");
                    if (f) {
                        fwrite(patched,4,pc2,f);
                        fclose(f);
                    }
                }
                STEREO_LOG("Pipe %u PathA: patch failed",p);
                continue;
            }
            if (dump) {
                uint64_t spv_hash = hash_spv(e->spv, e->words);
                char dp[512];
                _snprintf(
                    dp,
                    sizeof(dp)-1,
                    "%s\\%016llx+ts.spv",
                    dump,
                    (unsigned long long)spv_hash);
                FILE *f=fopen(dp,"wb");
                if (f) {
                    fwrite(patched,4,pc2,f);
                    fclose(f);
                }
            }
            VkShaderModuleCreateInfo smci={VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                NULL,0,pc2*4,patched};
            VkShaderModule tmp=VK_NULL_HANDLE;
            VkResult mr=sd->real.CreateShaderModule(sd->real_device,&smci,NULL,&tmp);
            spirv_patched_free(patched);
            if (mr!=VK_SUCCESS) {
                STEREO_ERR("Pipe %u PathA: module err %d",p,mr); continue; }
            uint32_t sc=ci->stageCount;
            VkPipelineShaderStageCreateInfo *st=malloc(sc*sizeof(*st));
            if (!st) { sd->real.DestroyShaderModule(sd->real_device,tmp,NULL); continue; }
            memcpy(st,ci->pStages,sc*sizeof(*st));
            st[tes_stage].module = tmp;
            infos[p].pStages = st;
            tmp_mod[p] = tmp;
            tst[p] = st;
            STEREO_LOG(
                "PATCHED_STAGE PathA p=%u stage=%u orig=%p patched=%p",
                p,
                tes_stage,
                (void *)ci->pStages[tes_stage].module,
                (void *)tmp);
            STEREO_LOG(
                "Pipe %u: Path A — TES patched (gl_ViewIndex)",
                p);
            continue;
        }
        STEREO_LOG(
        "PATHB_GATE p=%u in_mv=%d has_vs=%d has_tcs=%d vs_stage=%u",
        p,
        in_mv_rp,
        has_vs,
        has_tcs,
        vs_stage);
        /* ── Path B: patch VS with gl_ViewIndex ──────────────────────────
         * Only patch actual multiview render passes.
         * Non-multiview passes include deferred G-buffer, shadow, SSAO,
         * and post-processing passes that must remain center-eye.
         */
        if (in_mv_rp &&
            ci->stageCount > 0 &&
            has_vs &&
            !has_tcs &&
            vs_stage != ~0u) {
            StereoShaderCache *e=cache_find(sd, ci->pStages[vs_stage].module);
            if (!e) { STEREO_LOG("Pipe %u PathB: VS not cached",p); continue; }
            STEREO_LOG(
                "SHADER_MODULE stage=VS hash=%016llx words=%zu module=%p",
                (unsigned long long)hash_spv(e->spv, e->words),
                e->words,
                (void*)ci->pStages[vs_stage].module);
            STEREO_LOG(
                "VS_CONTEXT hash=%016llx rp=%p mv=%d color=%p depth=%d",
                (unsigned long long)hash_spv(e->spv, e->words),
                (void*)ci->renderPass,
                in_mv_rp,
                (void*)ci->renderPass,
                (ci->pDepthStencilState != NULL));
            if (dump)
            {
                uint64_t spv_hash=hash_spv(e->spv, e->words);
                char dp[512];
                _snprintf(
                    dp,
                    sizeof(dp)-1,
                    "%s\\%016llx-vs.spv",
                    dump,
                    (unsigned long long)spv_hash);
                FILE *f = fopen(dp, "rb");
                if (!f)
                {
                    f = fopen(dp, "wb");
                    if (f)
                    {
                        fwrite(
                            e->spv,
                            4,
                            e->words,
                            f);
                        fclose(f);
                    }
                }
                else
                {
                    fclose(f);
                }
            }
            uint32_t *patched=NULL; size_t pc2=0;
            STEREO_LOG(
                "[CALL B] lo=%f ro=%f conv=%f flip=%d",
                lo,
                ro,
                conv,
                sd->stereo.flip_eyes);
            STEREO_LOG(
                "PATCH_CONSTS lo=%f ro=%f conv=%f",
                lo,
                ro,
                conv);
            STEREO_LOG(
                "[CALL B] multiview=%d pass_exists=%d",
                sd->stereo.multiview,
                sd->multiview_pass_exists);
            STEREO_LOG(
                "PathB candidate module=%p words=%zu",
                (void*)ci->pStages[vs_stage].module,
                e->words);
            StereoDebugCtx *dbgB = &dbg_out[p];
            *dbgB = (StereoDebugCtx){
                p,
                ci->renderPass,
                in_mv_rp,
                (uint32_t)VK_SHADER_STAGE_VERTEX_BIT,
                0,
                9,
                false,
                false
            };
            if (!spirv_patch_stereo_vertex(
                    &sd->stereo,
                    e->spv, e->words,
                    &patched, &pc2,
                    lo, ro, conv,
                    /*inj_vi=*/true,
                    dbgB)) {
                STEREO_LOG("Pipe %u PathB: VS patch failed",p); continue; }
            if (dump) {
                uint64_t spv_hash = hash_spv(e->spv, e->words);
                char dp[512];
                _snprintf(
                    dp,
                    sizeof(dp)-1,
                    "%s\\%016llx+vs.spv",
                    dump,
                    (unsigned long long)spv_hash);
                FILE *f=fopen(dp,"wb");
                if (f) {
                    fwrite(patched,4,pc2,f);
                    fclose(f);
                }
            }
            VkShaderModuleCreateInfo smci={VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                NULL,0,pc2*4,patched};
            VkShaderModule tmp=VK_NULL_HANDLE;
            VkResult mr=sd->real.CreateShaderModule(sd->real_device,&smci,NULL,&tmp);
            spirv_patched_free(patched);
            if (mr!=VK_SUCCESS) {
                STEREO_ERR("Pipe %u PathB: VS module err %d",p,mr); continue; }
            uint32_t sc=ci->stageCount;
            VkPipelineShaderStageCreateInfo *st=malloc(sc*sizeof(*st));
            if (!st) { sd->real.DestroyShaderModule(sd->real_device,tmp,NULL); continue; }
            memcpy(st,ci->pStages,sc*sizeof(*st));
            st[vs_stage].module = tmp;
            infos[p].pStages = st;
            tmp_mod[p] = tmp;
            tst[p] = st;
            STEREO_LOG(
                "PATCHED_STAGE PathB p=%u stage=%u orig=%p patched=%p",
                p,
                vs_stage,
                (void *)ci->pStages[vs_stage].module,
                (void *)tmp);
            STEREO_LOG(
                "Pipe %u: Path B — VS gl_ViewIndex patch",
                p);
            continue;
        }
        STEREO_LOG("Pipe %u: no patchable VS/TES stage (stageCount=%u has_vs=%d has_tes=%d has_tcs=%d) — not patched",
                   p, ci->stageCount, has_vs, has_tes, has_tcs);
    }
    PIPE_DECISION_CONTINUE:
    /* ── PATCH 5: RenderPass-based multiview binding ─────────────── */
    for (uint32_t p = 0; p < N; p++) {
        StereoRenderPassInfo *rpi = NULL;
        if (pCI[p].renderPass != VK_NULL_HANDLE)
            rpi = stereo_rp_lookup(sd, pCI[p].renderPass);
        STEREO_LOG(
            "PIPE_RP p=%u ci_rp=%p rpi=%p has_mv=%u mv=%p",
            p,
            (void*)pCI[p].renderPass,
            (void*)rpi,
            rpi ? (unsigned)rpi->has_multiview : 0,
            rpi ? (void*)rpi->mv_handle : NULL);
        if (rpi && rpi->has_multiview) {
            STEREO_LOG("Pipe %u: binding MV render pass %p", p, (void*)rpi->mv_handle);
            infos[p].renderPass = rpi->mv_handle;
        }
    }
    for (uint32_t p = 0; p < N; p++) {
        STEREO_LOG(
            "PIPE_FINAL p=%u ci_rp=%p final_rp=%p stages=%u",
            p,
            (void*)pCI[p].renderPass,
            (void*)infos[p].renderPass,
            infos[p].stageCount);
    }
    for (uint32_t p = 0; p < N; ++p)
    {
        STEREO_LOG(
            "PIPE_CREATE pipeline=%u renderPass=%p subpass=%u",
            p,
            infos[p].renderPass,
            infos[p].subpass);
        for (uint32_t s = 0; s < infos[p].stageCount; s++)
        {
            STEREO_LOG(
                "PIPE_STAGE p=%u stage=%u vkstage=0x%x module=%p patched_tmp=%u",
                p,
                s,
                infos[p].pStages[s].stage,
                (void *)infos[p].pStages[s].module,
                (unsigned)(
                    tmp_mod[p] != VK_NULL_HANDLE &&
                    infos[p].pStages[s].module == tmp_mod[p]));
        }
    }
    STEREO_LOG(
        "[PIPE BEFORE DRIVER] N=%u",
        N);
    for (uint32_t p = 0; p < N; ++p)
    {
        STEREO_LOG(
            "[PIPE %u] patched=%d tmp=%p stages=%u",
            p,
            tst[p] != NULL,
            (void*)tmp_mod[p],
            infos[p].stageCount);
        for (uint32_t s = 0; s < infos[p].stageCount; ++s)
        {
            const VkPipelineShaderStageCreateInfo *st =
                &infos[p].pStages[s];
            STEREO_LOG(
                "    stage=%u module=%p stageFlags=0x%x",
                s,
                (void*)st->module,
                st->stage);
        }
    }
    VkResult res=sd->real.CreateGraphicsPipelines(sd->real_device,pc,N,infos,pAlloc,pP);
    STEREO_LOG(
        "[PIPE AFTER DRIVER] res=%d",
        res);
    for (uint32_t p = 0; p < N; p++) {
        STEREO_LOG(
            "PIPE_CREATED pipe=%p result=%d rp=%p orig_rp=%p stages=%u",
            (res == VK_SUCCESS) ? (void*)pP[p] : NULL,
            res,
            (void*)infos[p].renderPass,
            (void*)pCI[p].renderPass,
            infos[p].stageCount);
        if (res == VK_SUCCESS)
        {
            StereoPipelineInfo *info =
                add_pipeline_info(sd);
            if (info)
            {
                info->pipeline = pP[p];
                info->original_renderpass =
                    pCI[p].renderPass;
                info->mv_renderpass =
                    infos[p].renderPass;
                info->stage_count =
                    infos[p].stageCount;
                info->is_quad =
                    (!pCI[p].pVertexInputState ||
                     pCI[p].pVertexInputState->vertexBindingDescriptionCount == 0);
                
                info->vertex_binding_count =
                    pCI[p].pVertexInputState ?
                    pCI[p].pVertexInputState->vertexBindingDescriptionCount : 0;
                info->view_mask = 0; /* default */
                info->has_proj_ubo          = dbg_out[p].has_proj_ubo;
                info->proj_set              = dbg_out[p].proj_set;
                info->proj_binding          = dbg_out[p].proj_binding;
                info->proj_member_mask      = dbg_out[p].proj_member_mask;
                info->proj_var              = dbg_out[p].proj_var;
                for (uint32_t s = 0; s < infos[p].stageCount; s++)
                {
                    const VkPipelineShaderStageCreateInfo *st =
                        &infos[p].pStages[s];
                    if (st->stage == VK_SHADER_STAGE_VERTEX_BIT)
                    {
                        info->vs_module = st->module;
                        info->patched_vs =
                            (tmp_mod[p] != VK_NULL_HANDLE &&
                             st->module == tmp_mod[p]);
                    }
                    if (st->stage == VK_SHADER_STAGE_FRAGMENT_BIT)
                    {
                        info->fs_module = st->module;
                        info->patched_fs =
                            (tmp_mod[p] != VK_NULL_HANDLE &&
                             st->module == tmp_mod[p]);
                        /*
                         * Always probe the fragment shader too.
                         * VS/TES may already have filled proj info, but FS can carry
                         * its own projection UBO for SSAO / post-process paths.
                         */
                        StereoShaderCache *fs_cache = cache_find(sd, st->module);
                        if (fs_cache)
                        {
                            uint64_t h = hash_spv(fs_cache->spv, fs_cache->words);
                            STEREO_LOG(
                                "FS_PIPE_MODULE module=%p hash=%016llx words=%zu",
                                (void *)st->module,
                                (unsigned long long)h,
                                fs_cache->words);
                        }
                    }
                }
            }
            STEREO_LOG(
                "PIPE_INFO pipe=%p rp=%p orig_rp=%p stages=%u",
                (void*)pP[p],
                (void*)infos[p].renderPass,
                (void*)pCI[p].renderPass,
                infos[p].stageCount);
        }
    }
    STEREO_LOG(
        "PIPE_CREATE_END result=%d multiview_pass_exists=%d",
        res,
        sd->multiview_pass_exists);
    for (uint32_t p=0;p<N;p++) {
        if (tmp_mod[p]) {
            if (sd->tmp_module_count<MAX_TMP_MODULES)
                sd->tmp_modules[sd->tmp_module_count++]=tmp_mod[p];
            else
                sd->real.DestroyShaderModule(sd->real_device,tmp_mod[p],NULL);
        }
        free(tst[p]);
    }
    free(tmp_mod); free(tst); free(infos);
    return res;
}

/* ── vkDestroyShaderModule ───────────────────────────────────────────────── */
VKAPI_ATTR void VKAPI_CALL
stereo_DestroyShaderModule(VkDevice device, VkShaderModule sm,
                           const VkAllocationCallbacks *pAlloc)
{
    STEREO_LOG("CALLED stereo_DestroyShaderModule");
    StereoDevice *sd=stereo_device_from_handle(device);
    if (!sd) return;
    cache_remove(sd,sm);
    sd->real.DestroyShaderModule(sd->real_device,sm,pAlloc);
}
