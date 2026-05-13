/*
 * shader.c — SPIR-V patching for stereo vertex transform
 *
 * ────────────────────────────────────────────────────────────────────────────
 * Strategy
 * ────────────────────────────────────────────────────────────────────────────
 * We intercept vkCreateShaderModule and, if the module is a vertex shader,
 * we patch the SPIR-V to apply a per-eye clip-space offset to gl_Position.
 *
 * The patch injected after the last write to gl_Position:
 *
 *   // GLSL equivalent of what we inject in SPIR-V:
 *   float sign = (gl_ViewIndex == 0) ? -1.0 : 1.0;
 *   gl_Position.x += sign * stereoOffset * gl_Position.w;
 *
 * When inject_view_index=true (multiview=1 in vks3d.ini) and the shader
 * doesn't already have gl_ViewIndex, we inject:
 *   - OpCapability MultiView          (if not already declared)
 *   - OpDecorate %view_var BuiltIn 4440
 *   - OpTypePointer Input int         (if not already declared)
 *   - OpVariable %ptr_in_int %view_var Input
 *   - Extension of OpEntryPoint's interface list to include %view_var
 *
 * ────────────────────────────────────────────────────────────────────────────
 * SPIR-V patching approach (binary rewriting)
 * ────────────────────────────────────────────────────────────────────────────
 * 1. Parse SPIR-V header (magic, version, generator, bound, schema)
 * 2. Find the entry point with ExecutionModel = Vertex (0)
 * 3. Find (or insert) the gl_Position Output BuiltIn variable
 * 4. Find (or insert) gl_ViewIndex Input BuiltIn variable
 * 5. Find (or insert) a PushConstant struct with a float at offset 124
 * 6. Locate the last OpStore to gl_Position in the function body
 * 7. After that store, inject new SPIR-V instructions to:
 *      a. Load gl_Position
 *      b. Load gl_ViewIndex
 *      c. Compute offset = (viewIndex == 0) ? left_off : right_off
 *      d. Multiply by w component
 *      e. Add to x component
 *      f. Store back to gl_Position
 *
 * ────────────────────────────────────────────────────────────────────────────
 * NOTE: This is a simplified patcher targeting common SPIR-V patterns.
 * Full production use should employ a proper SPIR-V library (spirv-tools,
 * SPIRV-Cross, or LLVM SPIR-V backend).
 * ────────────────────────────────────────────────────────────────────────────
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include "stereo_icd.h"

/* ── SPIR-V opcodes we care about ─────────────────────────────────────────── */
#define SpvOpCapability              17
#define SpvOpExtension               10
#define SpvOpExtInstImport           11
#define SpvOpMemoryModel             14
#define SpvOpEntryPoint              15
#define SpvOpExecutionMode           16
#define SpvOpTypeVoid                19
#define SpvOpTypeBool                20
#define SpvOpTypeInt                 21
#define SpvOpTypeFloat               22
#define SpvOpTypeVector              23
#define SpvOpTypePointer             32
#define SpvOpTypeFunction            33
#define SpvOpTypeStruct              30
#define SpvOpTypeArray               28
#define SpvOpConstant                43
#define SpvOpConstantComposite       44
#define SpvOpConstantTrue            41
#define SpvOpConstantFalse           42
#define SpvOpVariable                59
#define SpvOpLoad                    61
#define SpvOpStore                   62
#define SpvOpAccessChain             65
#define SpvOpDecorate                71
#define SpvOpMemberDecorate          72
#define SpvOpFunction                54
#define SpvOpFunctionEnd             56
#define SpvOpLabel                   248
#define SpvOpBranch                  249
#define SpvOpReturn                  253
#define SpvOpCompositeExtract        81
#define SpvOpCompositeInsert         82
#define SpvOpFAdd                    129
#define SpvOpFMul                    133
#define SpvOpFNegate                 127
#define SpvOpIEqual                  170
#define SpvOpSelect                  169
#define SpvOpConvertUToF             112
#define SpvOpSConvertToF             111
#define SpvOpBitcast                 124

#define SpvDecorationBuiltIn         11
#define SpvDecorationLocation        30
#define SpvDecorationBinding         33
#define SpvDecorationOffset          35
#define SpvDecorationBlock           2

#define SpvBuiltInPosition           0
#define SpvBuiltInViewIndex          4440  /* SPV_KHR_multiview */

#define SpvStorageClassInput         1
#define SpvStorageClassOutput        3
#define SpvStorageClassPushConstant  9

#define SpvExecutionModelVertex      0

/* Capability MultiView = 5296 (SPV_KHR_multiview) */
#define SpvCapabilityMultiView       5296

#define SPIRV_MAGIC 0x07230203u

/* ── SPIR-V word buffer (dynamic array) ──────────────────────────────────── */
typedef struct SpvBuf {
    uint32_t *words;
    size_t    count;
    size_t    capacity;
} SpvBuf;

static bool spvbuf_init(SpvBuf *b, size_t initial)
{
    b->words    = malloc(initial * sizeof(uint32_t));
    b->count    = 0;
    b->capacity = initial;
    return b->words != NULL;
}

static bool spvbuf_push(SpvBuf *b, uint32_t w)
{
    if (b->count >= b->capacity) {
        size_t nc = b->capacity * 2;
        uint32_t *nw = realloc(b->words, nc * sizeof(uint32_t));
        if (!nw) return false;
        b->words    = nw;
        b->capacity = nc;
    }
    b->words[b->count++] = w;
    return true;
}

static bool spvbuf_push_n(SpvBuf *b, const uint32_t *ws, size_t n)
{
    for (size_t i = 0; i < n; i++)
        if (!spvbuf_push(b, ws[i])) return false;
    return true;
}

static void spvbuf_free(SpvBuf *b)
{
    free(b->words);
    b->words    = NULL;
    b->count    = 0;
    b->capacity = 0;
}

/* SPIR-V instruction word: [wordcount|opcode] */
static inline uint32_t spv_op(uint32_t opcode, uint32_t wordcount)
{
    return (wordcount << 16) | opcode;
}

/* ── SPIR-V module parser ──────────────────────────────────────────────── */
typedef struct SpvModule {
    const uint32_t *words;
    size_t          count;
    uint32_t        bound;     /* current ID bound, we extend it */
    /* Parsed info */
    bool            is_vertex; /* has Vertex entry point              */
    bool            has_mv_capability; /* OpCapability MultiView present */

    /* gl_Position — two discovery paths:
     *
     * (A) Direct:  OpDecorate %var BuiltIn Position
     *     pos_var = %var, pos_is_block = false
     *     Load/store pos_var directly.
     *
     * (B) Block member (standard GLSL compilation):
     *     OpMemberDecorate %struct member BuiltIn Position
     *     OpTypePointer Output %struct  →  pos_ptr_type
     *     OpVariable pos_ptr_type Output →  pos_var
     *     pos_is_block = true
     *     Access: OpAccessChain %ptr_v4 %pos_var %int_const_0 → chain
     *             then OpLoad/OpStore chain.
     */
    uint32_t        pos_var;       /* variable ID (both paths)            */
    bool            pos_is_block;  /* true = use AccessChain to member    */
    uint32_t        pos_block_type;/* struct type ID (path B)             */
    uint32_t        pos_member_idx;/* member index in struct (path B)     */
    uint32_t        pos_ptr_type;  /* OpTypePointer Output struct (B)     */

    uint32_t        view_var;  /* ID of gl_ViewIndex variable         */
    uint32_t        float_type;/* ID of OpTypeFloat 32                */
    uint32_t        v4_type;   /* ID of OpTypeVector float 4          */
    uint32_t        int_type;  /* ID of OpTypeInt 32 0 (unsigned)     */
    uint32_t        bool_type; /* ID of OpTypeBool                    */

    /* Pre-existing pointer types found in the shader — reuse these instead
     * of declaring new ones to avoid duplicate type-declaration errors. */
    uint32_t        ptr_out_v4_type;  /* existing OpTypePointer Output v4  (or 0) */
    uint32_t        ptr_in_int_type;  /* existing OpTypePointer Input  int (or 0) */
    /* Positions of key sections in word stream */
    size_t          first_function_word; /* start of first OpFunction  */
    size_t          header_end;          /* end of type/decoration section */
} SpvModule;

static void spirv_scan_pass(SpvModule *m, bool second_pass)
{
    const uint32_t *w = m->words;
    size_t          i = 5;

    while (i < m->count) {
        uint32_t op_code = w[i] & 0xffff;
        uint32_t wcount  = w[i] >> 16;
        if (wcount == 0 || i + wcount > m->count) break;

        if (!second_pass) {
            switch (op_code) {
            case SpvOpCapability:
                if (wcount >= 2 && w[i+1] == SpvCapabilityMultiView)
                    m->has_mv_capability = true;
                break;
            case SpvOpEntryPoint:
                if (wcount >= 2 && w[i+1] == SpvExecutionModelVertex)
                    m->is_vertex = true;
                break;
            case SpvOpTypeFloat:
                if (wcount == 3 && w[i+2] == 32)
                    m->float_type = w[i+1];
                break;
            case SpvOpTypeVector:
                if (wcount == 4 && w[i+2] == m->float_type && w[i+3] == 4)
                    m->v4_type = w[i+1];
                break;
            case SpvOpTypeInt:
                if (wcount == 4 && w[i+2] == 32)
                    m->int_type = w[i+1];
                break;
            case SpvOpTypeBool:
                if (wcount == 2)
                    m->bool_type = w[i+1];
                break;
            case SpvOpTypePointer:
                if (wcount >= 4) {
                    if (w[i+2] == SpvStorageClassOutput && m->v4_type &&
                        w[i+3] == m->v4_type)
                        m->ptr_out_v4_type = w[i+1];
                    if (w[i+2] == SpvStorageClassInput && m->int_type &&
                        w[i+3] == m->int_type)
                        m->ptr_in_int_type = w[i+1];
                }
                break;
            case SpvOpDecorate:
                if (wcount >= 4 &&
                    w[i+2] == SpvDecorationBuiltIn &&
                    w[i+3] == SpvBuiltInPosition &&
                    !m->pos_is_block)
                    m->pos_var = w[i+1];
                if (wcount >= 4 &&
                    w[i+2] == SpvDecorationBuiltIn &&
                    w[i+3] == SpvBuiltInViewIndex)
                    m->view_var = w[i+1];
                break;
            case SpvOpMemberDecorate:
                if (wcount >= 5 &&
                    w[i+3] == SpvDecorationBuiltIn &&
                    w[i+4] == SpvBuiltInPosition) {
                    m->pos_block_type = w[i+1];
                    m->pos_member_idx = w[i+2];
                    m->pos_is_block   = true;
                    m->pos_var        = 0;
                }
                break;
            case SpvOpFunction:
                if (m->first_function_word == 0)
                    m->first_function_word = i;
                break;
            default:
                break;
            }
        } else {
            switch (op_code) {
            case SpvOpTypePointer:
                if (wcount >= 4 &&
                    w[i+2] == SpvStorageClassOutput &&
                    m->pos_block_type != 0 &&
                    w[i+3] == m->pos_block_type)
                    m->pos_ptr_type = w[i+1];
                break;
            case SpvOpVariable:
                if (wcount >= 4 &&
                    w[i+3] == SpvStorageClassOutput &&
                    m->pos_ptr_type != 0 &&
                    w[i+1] == m->pos_ptr_type)
                    m->pos_var = w[i+2];
                break;
            default:
                break;
            }
        }
        i += wcount;
    }
}

static void spirv_scan(SpvModule *m)
{
    m->bound = m->words[3];
    spirv_scan_pass(m, false);
    if (m->pos_is_block)
        spirv_scan_pass(m, true);
}

/* ── Inject stereo offset code ───────────────────────────────────────────────
 *
 * Pushes type-section declarations then body instructions into `out`.
 * The caller splits them at the first body opcode.
 *
 * If inject_view_index is true and m->view_var is 0 (shader has no
 * gl_ViewIndex), we inject the variable declarations here and return its
 * ID in *out_injected_view_var so the caller can update OpEntryPoint.
 */
static bool inject_stereo_code(
    SpvBuf   *out,
    SpvModule *m,
    uint32_t  *next_id,
    float      left_off,
    float      right_off,
    bool       inject_view_index,
    uint32_t  *out_injected_view_var)
{
    if (!m->float_type || !m->v4_type || !m->pos_var)
        return false;

    /* Will we have a view_var available (existing or injected)? */
    bool will_inject_view = inject_view_index && !m->view_var && m->int_type;
    bool will_have_view   = m->view_var || will_inject_view;

    /* ── Allocate all IDs up front ─────────────────────────────────────── */
    uint32_t id_ptr_v4_out    = (*next_id)++;
    uint32_t id_ptr_int_in    = (*next_id)++;  /* may be used for view var too */
    uint32_t id_injected_view = will_inject_view ? (*next_id)++ : 0;
    uint32_t id_chain         = (*next_id)++;
    uint32_t id_load_pos      = (*next_id)++;
    uint32_t id_load_view     = will_have_view ? (*next_id)++ : 0;
    uint32_t id_const_zero_i  = (*next_id)++;
    uint32_t id_is_left       = will_have_view ? (*next_id)++ : 0;
    uint32_t id_const_l       = (*next_id)++;
    uint32_t id_const_r       = (*next_id)++;
    uint32_t id_off_sel       = (*next_id)++;
    uint32_t id_pos_w         = (*next_id)++;
    uint32_t id_delta         = (*next_id)++;
    uint32_t id_pos_x         = (*next_id)++;
    uint32_t id_new_x         = (*next_id)++;
    uint32_t id_new_pos       = (*next_id)++;

    /* Resolve which pointer type IDs to use (prefer existing ones) */
    uint32_t use_ptr_v4_out = m->ptr_out_v4_type ? m->ptr_out_v4_type : id_ptr_v4_out;
    uint32_t use_ptr_int_in = m->ptr_in_int_type  ? m->ptr_in_int_type  : id_ptr_int_in;
    uint32_t int_type       = m->int_type;

    /* Bool type — may need to declare if absent and we need branching */
    uint32_t bool_type = m->bool_type;
    uint32_t id_new_bool = 0;
    if (!bool_type && will_have_view && int_type) {
        id_new_bool = (*next_id)++;
        bool_type   = id_new_bool;
    }

    /* ── TYPE SECTION DECLARATIONS ─────────────────────────────────────── */

    /* OpTypePointer Output v4 — only if not already declared */
    if (!m->ptr_out_v4_type) {
        uint32_t w[] = { spv_op(SpvOpTypePointer, 4), id_ptr_v4_out,
                         SpvStorageClassOutput, m->v4_type };
        spvbuf_push_n(out, w, 4);
    }

    /* OpTypePointer Input int — only if not already declared */
    if (int_type && !m->ptr_in_int_type) {
        uint32_t w[] = { spv_op(SpvOpTypePointer, 4), id_ptr_int_in,
                         SpvStorageClassInput, int_type };
        spvbuf_push_n(out, w, 4);
        /* Mark as now emitted so injection below reuses the same ID */
        m->ptr_in_int_type = id_ptr_int_in;
        use_ptr_int_in = id_ptr_int_in;
    }

    /* OpConstant int 0 (used for both AccessChain index and IEqual) */
    if (int_type) {
        uint32_t w[] = { spv_op(SpvOpConstant, 4), int_type, id_const_zero_i, 0u };
        spvbuf_push_n(out, w, 4);
    }

    /* OpTypeBool — only if absent and needed */
    if (id_new_bool) {
        uint32_t w[] = { spv_op(SpvOpTypeBool, 2), id_new_bool };
        spvbuf_push_n(out, w, 2);
    }

    /* OpConstant float left_off */
    {
        uint32_t w[4] = { spv_op(SpvOpConstant, 4), m->float_type, id_const_l, 0u };
        memcpy(&w[3], &left_off, sizeof(float));
        spvbuf_push_n(out, w, 4);
    }

    /* OpConstant float right_off */
    {
        uint32_t w[4] = { spv_op(SpvOpConstant, 4), m->float_type, id_const_r, 0u };
        memcpy(&w[3], &right_off, sizeof(float));
        spvbuf_push_n(out, w, 4);
    }

    /* ── INJECT gl_ViewIndex VARIABLE (if needed) ─────────────────────── *
     * Placed after type declarations so use_ptr_int_in is resolved.       *
     * In strict SPIR-V, OpDecorate should be in the annotation section    *
     * and OpVariable in the global-variable section.  Both land here in   *
     * the "before-first-OpFunction" region; drivers tolerate the order.   */
    if (will_inject_view) {
        /* OpDecorate %view_var BuiltIn ViewIndex */
        uint32_t deco[] = { spv_op(SpvOpDecorate, 4),
                            id_injected_view, SpvDecorationBuiltIn, SpvBuiltInViewIndex };
        spvbuf_push_n(out, deco, 4);

        /* OpVariable ptr_in_int %view_var Input */
        uint32_t var[] = { spv_op(SpvOpVariable, 4),
                           use_ptr_int_in, id_injected_view, SpvStorageClassInput };
        spvbuf_push_n(out, var, 4);

        /* Update module so body code uses the new variable */
        m->view_var = id_injected_view;
        if (out_injected_view_var) *out_injected_view_var = id_injected_view;
    } else {
        if (out_injected_view_var) *out_injected_view_var = 0;
    }

    /* ── BODY INSTRUCTIONS ─────────────────────────────────────────────── */

    /* Determine pos_ptr: AccessChain for block path, direct var for path A */
    uint32_t pos_ptr;
    if (m->pos_is_block) {
        if (!int_type) {
            STEREO_LOG("inject_stereo_code: no int type for AccessChain");
            return false;
        }
        uint32_t ac[] = { spv_op(SpvOpAccessChain, 5),
                          use_ptr_v4_out, id_chain, m->pos_var, id_const_zero_i };
        spvbuf_push_n(out, ac, 5);
        pos_ptr = id_chain;
    } else {
        pos_ptr = m->pos_var;
        (void)id_chain;
    }

    /* OpLoad vec4 id_load_pos = *pos_ptr */
    {
        uint32_t w[] = { spv_op(SpvOpLoad, 4), m->v4_type, id_load_pos, pos_ptr };
        spvbuf_push_n(out, w, 4);
    }

    /* View-index branch or constant path */
    if (m->view_var && int_type && bool_type) {
        /* OpLoad int id_load_view = *view_var */
        uint32_t lv[] = { spv_op(SpvOpLoad, 4), int_type, id_load_view, m->view_var };
        spvbuf_push_n(out, lv, 4);

        /* OpIEqual bool id_is_left = (id_load_view == 0) */
        uint32_t eq[] = { spv_op(SpvOpIEqual, 5), bool_type, id_is_left,
                          id_load_view, id_const_zero_i };
        spvbuf_push_n(out, eq, 5);

        /* OpSelect float id_off_sel = is_left ? left_off : right_off */
        uint32_t sel[] = { spv_op(SpvOpSelect, 6), m->float_type, id_off_sel,
                           id_is_left, id_const_l, id_const_r };
        spvbuf_push_n(out, sel, 6);
    } else {
        /* No view index — both eyes get the same (left) offset.
         * id_off_sel aliases id_const_l; nothing to emit. */
        id_off_sel = id_const_l;
        (void)id_load_view; (void)id_is_left;
    }

    /* OpCompositeExtract float id_pos_w = pos[3] */
    {
        uint32_t w[] = { spv_op(SpvOpCompositeExtract, 5),
                         m->float_type, id_pos_w, id_load_pos, 3u };
        spvbuf_push_n(out, w, 5);
    }

    /* OpFMul float id_delta = id_off_sel * id_pos_w */
    {
        uint32_t w[] = { spv_op(SpvOpFMul, 5),
                         m->float_type, id_delta, id_off_sel, id_pos_w };
        spvbuf_push_n(out, w, 5);
    }

    /* OpCompositeExtract float id_pos_x = pos[0] */
    {
        uint32_t w[] = { spv_op(SpvOpCompositeExtract, 5),
                         m->float_type, id_pos_x, id_load_pos, 0u };
        spvbuf_push_n(out, w, 5);
    }

    /* OpFAdd float id_new_x = id_pos_x + id_delta */
    {
        uint32_t w[] = { spv_op(SpvOpFAdd, 5),
                         m->float_type, id_new_x, id_pos_x, id_delta };
        spvbuf_push_n(out, w, 5);
    }

    /* OpCompositeInsert vec4 id_new_pos = pos with [0]=id_new_x */
    {
        uint32_t w[] = { spv_op(SpvOpCompositeInsert, 6),
                         m->v4_type, id_new_pos, id_new_x, id_load_pos, 0u };
        spvbuf_push_n(out, w, 6);
    }

    /* OpStore *pos_ptr = id_new_pos */
    {
        uint32_t w[] = { spv_op(SpvOpStore, 3), pos_ptr, id_new_pos };
        spvbuf_push_n(out, w, 3);
    }

    return true;
}

/* ── Main SPIR-V patcher ─────────────────────────────────────────────────── */
bool spirv_patch_stereo_vertex(
    const uint32_t  *in_words,
    size_t           in_count,
    uint32_t       **out_words,
    size_t          *out_count,
    float            left_offset,
    float            right_offset,
    float            convergence,
    bool             inject_view_index)
{
    (void)convergence;

    if (!in_words || in_count < 5) return false;
    if (in_words[0] != SPIRV_MAGIC)  return false;

    SpvModule m;
    memset(&m, 0, sizeof(m));
    m.words = in_words;
    m.count = in_count;
    spirv_scan(&m);

    if (!m.is_vertex) {
        STEREO_LOG("Shader scan: is_vertex=0, skipping patch");
        return false;
    }
    STEREO_LOG("Shader scan: is_vertex=%d is_block=%d block_type=%u ptr_type=%u "
               "pos_var=%u member=%u float_type=%u v4=%u int_type=%u view_var=%u "
               "ptr_out_v4=%u ptr_in_int=%u bound=%u words=%zu has_mv_cap=%d",
               (int)m.is_vertex, (int)m.pos_is_block,
               m.pos_block_type, m.pos_ptr_type, m.pos_var, m.pos_member_idx,
               m.float_type, m.v4_type, m.int_type, m.view_var,
               m.ptr_out_v4_type, m.ptr_in_int_type, m.bound, m.count,
               (int)m.has_mv_capability);

    if (!m.pos_var) {
        STEREO_LOG("Shader scan: gl_Position NOT found — skipping patch");
        return false;
    }

    STEREO_LOG("Patching vertex shader: pos_var=%u view_var=%u inject_view=%d bound=%u",
               m.pos_var, m.view_var, (int)inject_view_index, m.bound);

    /* ── Build combined type+body buffer via inject_stereo_code ─────────── */
    SpvBuf combined;
    if (!spvbuf_init(&combined, 256)) return false;

    uint32_t next_id = m.bound;
    uint32_t injected_view_var = 0;

    if (!inject_stereo_code(&combined, &m, &next_id,
                            left_offset, right_offset,
                            inject_view_index, &injected_view_var)) {
        STEREO_LOG("Stereo code injection skipped (missing types)");
        spvbuf_free(&combined);
        return false;
    }

    /* ── Split combined buffer into type_extras and body_extras ──────────── */
    SpvBuf type_extras, body_extras;
    if (!spvbuf_init(&type_extras, 64) || !spvbuf_init(&body_extras, 64)) {
        spvbuf_free(&combined);
        spvbuf_free(&type_extras);
        spvbuf_free(&body_extras);
        return false;
    }

    size_t type_split = 0;
    for (size_t i = 0; i < combined.count; ) {
        uint32_t op = combined.words[i] & 0xffff;
        if (op == SpvOpLoad           ||
            op == SpvOpStore          ||
            op == SpvOpAccessChain    ||
            op == SpvOpIEqual         ||
            op == SpvOpSelect         ||
            op == SpvOpFMul           ||
            op == SpvOpFAdd           ||
            op == SpvOpCompositeExtract ||
            op == SpvOpCompositeInsert) {
            type_split = i;
            break;
        }
        uint32_t wc = combined.words[i] >> 16;
        if (!wc) break;
        i += wc;
        type_split = i;
    }

    for (size_t i = 0; i < type_split; i++)
        spvbuf_push(&type_extras, combined.words[i]);
    for (size_t i = type_split; i < combined.count; i++)
        spvbuf_push(&body_extras, combined.words[i]);
    spvbuf_free(&combined);

    /* ── Find insertion points ───────────────────────────────────────────── */
    size_t insert_type_after = 0;
    size_t insert_body_after = 0;
    {
        size_t i = 5;
        while (i < in_count) {
            uint32_t op     = in_words[i] & 0xffff;
            uint32_t wcount = in_words[i] >> 16;
            if (!wcount || i + wcount > in_count) break;
            if (op == SpvOpFunction && insert_type_after == 0)
                insert_type_after = i;
            if (!m.pos_is_block &&
                op == SpvOpStore && wcount >= 3 && in_words[i+1] == m.pos_var)
                insert_body_after = i + wcount;
            i += wcount;
        }
    }

    if (insert_type_after == 0) {
        STEREO_LOG("No OpFunction found — skipping patch");
        spvbuf_free(&type_extras);
        spvbuf_free(&body_extras);
        return false;
    }

    if (insert_body_after == 0) {
        if (m.pos_is_block)
            STEREO_LOG("Block-member Position: injecting before first OpReturn");
        else
            STEREO_LOG("No direct OpStore to gl_Position; injecting before first OpReturn");
        size_t i = 5;
        while (i < in_count) {
            uint32_t op     = in_words[i] & 0xffff;
            uint32_t wcount = in_words[i] >> 16;
            if (!wcount) break;
            if (op == SpvOpReturn || op == 254 /* ReturnValue */) {
                insert_body_after = i;
                break;
            }
            i += wcount;
        }
        if (!insert_body_after) {
            STEREO_LOG("No OpReturn found — skipping patch");
            spvbuf_free(&type_extras);
            spvbuf_free(&body_extras);
            return false;
        }
    }

    if (insert_body_after < insert_type_after) {
        STEREO_LOG("insert_body_after(%zu) < insert_type_after(%zu) — skipping",
                   insert_body_after, insert_type_after);
        spvbuf_free(&type_extras);
        spvbuf_free(&body_extras);
        return false;
    }

    /* ── Pass 2: Rebuild output ──────────────────────────────────────────── *
     *                                                                         *
     * Three special transformations during the copy:                          *
     *                                                                         *
     * 1. OpCapability MultiView — if we injected gl_ViewIndex and the shader  *
     *    doesn't already declare this capability, emit it as the very first   *
     *    instruction so it lands in the capability section.                   *
     *                                                                         *
     * 2. OpEntryPoint (vertex) — if we injected a new Input variable, extend  *
     *    the instruction's interface list with the new variable ID.  SPIR-V   *
     *    1.4 §2.4 rule 4 requires every statically-used Input/Output variable *
     *    to appear in the OpEntryPoint interface; missing it causes black      *
     *    output at runtime on strict drivers (ICD silently discards writes).   *
     *                                                                         *
     * 3. Splice points — type_extras before first OpFunction, body_extras at  *
     *    insert_body_after (existing logic).                                   */

    bool need_mv_cap  = injected_view_var && !m.has_mv_capability;
    bool mv_cap_done  = false;

    SpvBuf out_buf;
    if (!spvbuf_init(&out_buf,
                     in_count + type_extras.count + body_extras.count +
                     (need_mv_cap ? 2 : 0) +
                     (injected_view_var ? 1 : 0) + 16)) {
        spvbuf_free(&type_extras);
        spvbuf_free(&body_extras);
        return false;
    }

    /* Copy 5-word header, updating the ID bound */
    spvbuf_push_n(&out_buf, in_words, 5);
    out_buf.words[3] = next_id;

    for (size_t i = 5; i < in_count; ) {
        /* 1. Inject OpCapability MultiView before the very first instruction */
        if (!mv_cap_done && need_mv_cap) {
            uint32_t cap[] = { spv_op(SpvOpCapability, 2), SpvCapabilityMultiView };
            spvbuf_push_n(&out_buf, cap, 2);
            mv_cap_done = true;
        }

        /* 2. Splice type_extras just before first OpFunction */
        if (i == insert_type_after && type_extras.count > 0) {
            spvbuf_push_n(&out_buf, type_extras.words, type_extras.count);
            type_extras.count = 0;
        }

        /* 3. Splice body_extras at target position */
        if (i == insert_body_after && body_extras.count > 0) {
            spvbuf_push_n(&out_buf, body_extras.words, body_extras.count);
            body_extras.count = 0;
        }

        uint32_t op     = in_words[i] & 0xffff;
        uint32_t wcount = in_words[i] >> 16;
        if (!wcount || i + wcount > in_count) break;

        /* 4. Extend OpEntryPoint for vertex shader with the injected variable */
        if (op == SpvOpEntryPoint &&
            wcount >= 4 &&
            in_words[i+1] == SpvExecutionModelVertex &&
            injected_view_var != 0) {
            /* Emit with wordcount+1 and append the new interface variable */
            spvbuf_push(&out_buf, ((wcount + 1) << 16) | SpvOpEntryPoint);
            spvbuf_push_n(&out_buf, &in_words[i+1], wcount - 1);
            spvbuf_push(&out_buf, injected_view_var);
            STEREO_LOG("OpEntryPoint extended: added view_var=%u to interface list",
                       injected_view_var);
            i += wcount;
            continue;
        }

        /* Normal copy */
        spvbuf_push_n(&out_buf, &in_words[i], wcount);
        i += wcount;
    }

    /* Flush any remaining extras whose insertion point was == in_count */
    if (type_extras.count > 0)
        spvbuf_push_n(&out_buf, type_extras.words, type_extras.count);
    if (body_extras.count > 0)
        spvbuf_push_n(&out_buf, body_extras.words, body_extras.count);

    spvbuf_free(&type_extras);
    spvbuf_free(&body_extras);

    *out_words = out_buf.words;
    *out_count = out_buf.count;

    STEREO_LOG("SPIR-V patched: %zu -> %zu words, new bound=%u, "
               "view_var=%u (injected=%d), mv_cap_added=%d",
               in_count, out_buf.count, next_id,
               m.view_var, (int)(injected_view_var != 0), (int)mv_cap_done);
    return true;
}

void spirv_patched_free(uint32_t *words)
{
    free(words);
}

/* ── vkCreateShaderModule ────────────────────────────────────────────────── */
VKAPI_ATTR VkResult VKAPI_CALL
stereo_CreateShaderModule(
    VkDevice                         device,
    const VkShaderModuleCreateInfo  *pCreateInfo,
    const VkAllocationCallbacks     *pAllocator,
    VkShaderModule                  *pShaderModule)
{
    StereoDevice *sd = stereo_device_from_handle(device);
    if (!sd) return VK_ERROR_DEVICE_LOST;

    static int no_patch = -1;
    if (no_patch < 0) {
        const char *e = stereo_getenv("VKS3D_NO_SHADER_PATCH");
        no_patch = (e && e[0] == '1') ? 1 : 0;
        if (no_patch) STEREO_LOG("VKS3D_NO_SHADER_PATCH=1: SPIR-V patching disabled");
    }

    if (!sd->stereo.enabled || no_patch) {
        STEREO_LOG("stereo_CreateShaderModule: passthrough (%zu bytes)",
                   pCreateInfo ? pCreateInfo->codeSize : 0);
        VkResult r = sd->real.CreateShaderModule(sd->real_device, pCreateInfo,
                                                  pAllocator, pShaderModule);
        STEREO_LOG("stereo_CreateShaderModule: passthrough result=%d module=%p", r,
                   pShaderModule ? (void*)(uintptr_t)*pShaderModule : NULL);
        return r;
    }

    const uint32_t *in   = (const uint32_t*)pCreateInfo->pCode;
    size_t          in_c = pCreateInfo->codeSize / 4;
    uint32_t       *patched   = NULL;
    size_t          patched_c = 0;

    bool patched_ok = spirv_patch_stereo_vertex(
        in, in_c, &patched, &patched_c,
        sd->stereo.left_eye_offset,
        sd->stereo.right_eye_offset,
        sd->stereo.convergence,
        sd->stereo.multiview);

    VkShaderModuleCreateInfo mod_ci = *pCreateInfo;
    if (patched_ok) {
        mod_ci.pCode    = patched;
        mod_ci.codeSize = patched_c * sizeof(uint32_t);
        STEREO_LOG("stereo_CreateShaderModule: submitting patched SPIR-V (%zu words)",
                   patched_c);
    } else {
        STEREO_LOG("stereo_CreateShaderModule: submitting original SPIR-V (%zu words)",
                   in_c);
    }

    /* VKS3D_DUMP_SPIRV=C:\path\dir dumps patched SPIR-V for spirv-val inspection */
    {
        const char *dump_dir = stereo_getenv("VKS3D_DUMP_SPIRV");
        if (dump_dir && patched_ok) {
            static int dump_idx = 0;
            char dump_path[512];
            _snprintf(dump_path, sizeof(dump_path)-1, "%s\\patched_%04d.spv",
                      dump_dir, dump_idx++);
            FILE *f = fopen(dump_path, "wb");
            if (f) {
                fwrite(patched, sizeof(uint32_t), patched_c, f);
                fclose(f);
                STEREO_LOG("Dumped patched SPIR-V: %s (%zu words)", dump_path, patched_c);
            }
        }
    }

    VkResult res = sd->real.CreateShaderModule(
        sd->real_device, &mod_ci, pAllocator, pShaderModule);

    STEREO_LOG("stereo_CreateShaderModule: driver returned %d  module=%p",
               res, pShaderModule ? (void*)(uintptr_t)*pShaderModule : NULL);

    if (patched_ok) spirv_patched_free(patched);
    return res;
}

/* ── vkCreateGraphicsPipelines ───────────────────────────────────────────── */
VKAPI_ATTR VkResult VKAPI_CALL
stereo_CreateGraphicsPipelines(
    VkDevice                            device,
    VkPipelineCache                     pipelineCache,
    uint32_t                            createInfoCount,
    const VkGraphicsPipelineCreateInfo *pCreateInfos,
    const VkAllocationCallbacks        *pAllocator,
    VkPipeline                         *pPipelines)
{
    StereoDevice *sd = stereo_device_from_handle(device);
    if (!sd) return VK_ERROR_DEVICE_LOST;
    STEREO_LOG("stereo_CreateGraphicsPipelines: count=%u", createInfoCount);
    VkResult r = sd->real.CreateGraphicsPipelines(
        sd->real_device, pipelineCache, createInfoCount,
        pCreateInfos, pAllocator, pPipelines);
    STEREO_LOG("stereo_CreateGraphicsPipelines: result=%d", (int)r);
    return r;
}

/* ── vkDestroyShaderModule ───────────────────────────────────────────────── */
VKAPI_ATTR void VKAPI_CALL
stereo_DestroyShaderModule(
    VkDevice                        device,
    VkShaderModule                  shaderModule,
    const VkAllocationCallbacks    *pAllocator)
{
    StereoDevice *sd = stereo_device_from_handle(device);
    if (!sd) return;
    sd->real.DestroyShaderModule(sd->real_device, shaderModule, pAllocator);
}
