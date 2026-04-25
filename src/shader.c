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
 * where stereoOffset is loaded from a PushConstant (injected at offset 124
 * in the push constant block — we pick a late offset to avoid collisions
 * with common app push constants which tend to be small).
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
#define SpvOpDecoratieBuiltIn        71  /* uses Decoration=BuiltIn(11) */
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
     * of declaring new ones to avoid duplicate type-declaration errors that
     * cause strict validators (595.71+) to reject/crash on the patched SPIR-V. */
    uint32_t        ptr_out_v4_type;  /* existing OpTypePointer Output v4  (or 0) */
    uint32_t        ptr_in_int_type;  /* existing OpTypePointer Input  int (or 0) */
    /* Positions of key sections in word stream */
    size_t          first_function_word; /* start of first OpFunction  */
    size_t          header_end;          /* end of type/decoration section */
} SpvModule;

/*
 * spirv_scan — two-pass scanner for correctness.
 *
 * Problem: SPIR-V ordering guarantees that decorations (OpMemberDecorate)
 * appear in the Annotation section BEFORE type declarations (OpTypePointer,
 * OpVariable) in the Type-Declaration section.  In practice, glslangValidator
 * and DXC reliably honour this.  However, some SPIR-V optimisers or
 * hand-written shaders may reorder instructions within their allowed ranges,
 * placing OpTypePointer before pos_block_type has been established.  A single
 * forward pass then misses the pointer-type match and leaves pos_var == 0.
 *
 * Fix: two passes.
 *   Pass 1 — collect everything except pos_ptr_type / pos_var which depend on
 *             pos_block_type that may not yet be known.
 *   Pass 2 — with pos_block_type known, resolve OpTypePointer → pos_ptr_type
 *             and OpVariable → pos_var.
 */
static void spirv_scan_pass(SpvModule *m, bool second_pass)
{
    const uint32_t *w = m->words;
    size_t          i = 5;

    while (i < m->count) {
        uint32_t op_code = w[i] & 0xffff;
        uint32_t wcount  = w[i] >> 16;
        if (wcount == 0 || i + wcount > m->count) break;

        if (!second_pass) {
            /* ── Pass 1 ─────────────────────────────────────────────────── */
            switch (op_code) {
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
                /* Detect existing Output v4 pointer (for gl_Position) and
                 * Input int pointer (for gl_ViewIndex) so we can REUSE these
                 * instead of declaring duplicates, which are invalid SPIR-V. */
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
                /* Path A: OpDecorate %var BuiltIn Position */
                if (wcount >= 4 &&
                    w[i+2] == SpvDecorationBuiltIn &&
                    w[i+3] == SpvBuiltInPosition &&
                    !m->pos_is_block)
                {
                    m->pos_var = w[i+1];
                }
                /* gl_ViewIndex */
                if (wcount >= 4 &&
                    w[i+2] == SpvDecorationBuiltIn &&
                    w[i+3] == SpvBuiltInViewIndex)
                {
                    m->view_var = w[i+1];
                }
                break;
            case SpvOpMemberDecorate:
                /* Path B: OpMemberDecorate %struct member BuiltIn Position */
                if (wcount >= 5 &&
                    w[i+3] == SpvDecorationBuiltIn &&
                    w[i+4] == SpvBuiltInPosition)
                {
                    m->pos_block_type = w[i+1];
                    m->pos_member_idx = w[i+2];
                    m->pos_is_block   = true;
                    m->pos_var        = 0;  /* resolved in pass 2 */
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
            /* ── Pass 2: pos_block_type is now known ─────────────────────── */
            switch (op_code) {
            case SpvOpTypePointer:
                /* w[i+1]=result_id, w[i+2]=StorageClass, w[i+3]=pointed-type */
                if (wcount >= 4 &&
                    w[i+2] == SpvStorageClassOutput &&
                    m->pos_block_type != 0 &&
                    w[i+3] == m->pos_block_type)
                {
                    m->pos_ptr_type = w[i+1];
                }
                break;
            case SpvOpVariable:
                /* w[i+1]=result_type, w[i+2]=result_id, w[i+3]=StorageClass */
                if (wcount >= 4 &&
                    w[i+3] == SpvStorageClassOutput &&
                    m->pos_ptr_type != 0 &&
                    w[i+1] == m->pos_ptr_type)
                {
                    m->pos_var = w[i+2];
                }
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
    spirv_scan_pass(m, false);   /* pass 1: collect types, decorations */
    if (m->pos_is_block)
        spirv_scan_pass(m, true); /* pass 2: resolve ptr_type and pos_var */
}

/* ── Inject stereo offset code after the last OpStore to pos_var ─────────── */

/* Build instructions that compute and apply the stereo x-offset.
 * Emits into `out` (a SpvBuf being built).
 * Assumes pos_var, view_var, float_type, v4_type, int_type, bool_type are known.
 *
 * Injected code (GLSL pseudocode):
 *   vec4 pos = gl_Position;
 *   int  vi  = gl_ViewIndex;
 *   bool isLeft = (vi == 0);
 *   float off = isLeft ? left_offset : right_offset;   // constants
 *   pos.x += off * pos.w;
 *   gl_Position = pos;
 */
static bool inject_stereo_code(
    SpvBuf  *out,
    SpvModule *m,
    uint32_t *next_id,      /* in/out: next available SPIR-V ID */
    float     left_off,
    float     right_off)
{
    /* We need these types to be known */
    if (!m->float_type || !m->v4_type || !m->pos_var)
        return false;

    /* Allocate IDs — only allocate view-index related IDs when view_var is
     * present. Allocating an ID without defining it inflates the SPIR-V bound
     * and produces invalid SPIR-V; the driver accepts it at CreateShaderModule
     * but crashes during vkCreateGraphicsPipelines compilation. */
    uint32_t id_ptr_v4_out   = (*next_id)++;
    uint32_t id_ptr_int_in   = (*next_id)++;
    uint32_t id_chain        = (*next_id)++;
    uint32_t id_load_pos     = (*next_id)++;
    /* These are only defined in the output when m->view_var != 0 */
    uint32_t id_load_view    = m->view_var ? (*next_id)++ : 0;
    uint32_t id_const_zero_i = (*next_id)++;   /* also used for AccessChain index */
    uint32_t id_is_left      = m->view_var ? (*next_id)++ : 0;
    uint32_t id_const_l      = (*next_id)++;
    uint32_t id_const_r      = (*next_id)++;
    uint32_t id_off_sel      = (*next_id)++;
    uint32_t id_pos_w        = (*next_id)++;
    uint32_t id_delta        = (*next_id)++;
    uint32_t id_pos_x        = (*next_id)++;
    uint32_t id_new_x        = (*next_id)++;
    uint32_t id_new_pos      = (*next_id)++;

    /* ── Reuse existing pointer types where possible ─────────────────
     * Emitting a duplicate OpTypePointer (same storage class + pointee) is
     * invalid SPIR-V and causes strict validators (driver 595.71+) to reject
     * or crash on the patched shader.  Use the IDs already in the shader when
     * the scanner found them; only allocate + emit new ones when absent.     */
    uint32_t use_ptr_v4_out = m->ptr_out_v4_type
                              ? m->ptr_out_v4_type  /* reuse existing */
                              : id_ptr_v4_out;       /* emit new below */
    uint32_t int_type = m->int_type;
    uint32_t use_ptr_int_in = m->ptr_in_int_type
                              ? m->ptr_in_int_type
                              : id_ptr_int_in;

    /* ── Emit type declarations only for types NOT already in shader ──
     * These go in the type/decoration section BEFORE functions.         */

    /* OpTypePointer Output v4  — only if not already declared */
    uint32_t ptr_out_v4_words[] = {
        spv_op(SpvOpTypePointer, 4),
        id_ptr_v4_out,   /* new ID — only emitted when m->ptr_out_v4_type == 0 */
        SpvStorageClassOutput,
        m->v4_type
    };

    /* OpTypePointer Input int  — only if not already declared */
    uint32_t ptr_in_int_words[4] = {
        spv_op(SpvOpTypePointer, 4),
        id_ptr_int_in,
        SpvStorageClassInput,
        int_type ? int_type : 0
    };

    /* constant int 0 */
    uint32_t const_i0_words[] = {
        spv_op(SpvOpConstant, 4),
        int_type ? int_type : 0,
        id_const_zero_i,
        0u
    };

    /* constant float left_off */
    uint32_t const_l_words[4] = {
        spv_op(SpvOpConstant, 4),
        m->float_type,
        id_const_l,
        0u
    };
    memcpy(&const_l_words[3], &left_off, sizeof(float));

    /* constant float right_off */
    uint32_t const_r_words[4] = {
        spv_op(SpvOpConstant, 4),
        m->float_type,
        id_const_r,
        0u
    };
    memcpy(&const_r_words[3], &right_off, sizeof(float));

    /* ── Record which words are type-section vs function-body ──────── */
    /* Type declarations: ptr_out_v4, ptr_in_int, const_i0, const_l, const_r */
    /* Store in a small array tagged as "pre-function" */
    /* We return these as part of the output buffer already ordered correctly */

    /* If shader has no OpTypeBool, we need to emit one before the function */
    uint32_t bool_type = m->bool_type;
    uint32_t id_new_bool = 0;
    if (!bool_type && m->view_var && int_type) {
        /* Need a bool type for OpIEqual — declare it in the type section */
        id_new_bool = (*next_id)++;
        bool_type   = id_new_bool;
    }

    /* Emit type declarations (will be placed before first function by caller).
     * Only emit a new OpTypePointer when the shader doesn't already have one
     * with that storage class + pointee — duplicates are invalid SPIR-V.     */
    if (!m->ptr_out_v4_type)
        spvbuf_push_n(out, ptr_out_v4_words, 4);
    if (int_type) {
        if (!m->ptr_in_int_type)
            spvbuf_push_n(out, ptr_in_int_words, 4);
        spvbuf_push_n(out, const_i0_words, 4);
    }
    if (id_new_bool) {
        uint32_t bool_decl[] = { spv_op(SpvOpTypeBool, 2), id_new_bool };
        spvbuf_push_n(out, bool_decl, 2);
    }
    spvbuf_push_n(out, const_l_words, 4);
    spvbuf_push_n(out, const_r_words, 4);

    /* ── Function body instructions ────────────────────────────────────
     * Path B (block member): emit OpAccessChain to reach Position, then Load.
     * Path A (direct var):   emit OpLoad from pos_var directly.
     */
    uint32_t pos_ptr; /* the ID we Load from / Store to for Position */

    if (m->pos_is_block) {
        if (!int_type) {
            STEREO_LOG("inject_stereo_code: no int type for AccessChain index");
            return false;
        }
        /* OpAccessChain %ptr_v4_out %pos_var %const_zero_i  →  id_chain */
        uint32_t ac[] = {
            spv_op(SpvOpAccessChain, 5),
            use_ptr_v4_out, id_chain, m->pos_var, id_const_zero_i
        };
        spvbuf_push_n(out, ac, 5);
        pos_ptr = id_chain;
    } else {
        pos_ptr = m->pos_var;
        (void)id_chain;
    }

    /* OpLoad vec4 id_load_pos = *pos_ptr */
    uint32_t load_pos[] = { spv_op(SpvOpLoad, 4), m->v4_type, id_load_pos, pos_ptr };
    spvbuf_push_n(out, load_pos, 4);

    /* If we have view_var, load it; else default to left offset always */
    if (m->view_var && int_type) {
        /* OpLoad int id_load_view = *view_var */
        uint32_t load_view[] = { spv_op(SpvOpLoad, 4), int_type, id_load_view, m->view_var };
        spvbuf_push_n(out, load_view, 4);

        /* OpIEqual bool id_is_left = id_load_view == const_zero_i */
        uint32_t is_left[] = {
            spv_op(SpvOpIEqual, 5),
            bool_type,
            id_is_left,
            id_load_view,
            id_const_zero_i
        };
        spvbuf_push_n(out, is_left, 5);

        /* OpSelect float id_off_sel = is_left ? left_off : right_off */
        uint32_t sel[] = {
            spv_op(SpvOpSelect, 6),
            m->float_type,
            id_off_sel,
            id_is_left,
            id_const_l,
            id_const_r
        };
        spvbuf_push_n(out, sel, 6);
    } else {
        /* No view index available, just use left offset as a no-op default */
        /* OpCopyObject — treat id_off_sel as alias of id_const_l */
        id_off_sel = id_const_l;
    }

    /* OpCompositeExtract float id_pos_w = id_load_pos[3] */
    uint32_t ext_w[] = {
        spv_op(SpvOpCompositeExtract, 5),
        m->float_type,
        id_pos_w,
        id_load_pos,
        3u          /* component 3 = w */
    };
    spvbuf_push_n(out, ext_w, 5);

    /* OpFMul float id_delta = id_off_sel * id_pos_w */
    uint32_t fmul[] = {
        spv_op(SpvOpFMul, 5),
        m->float_type,
        id_delta,
        id_off_sel,
        id_pos_w
    };
    spvbuf_push_n(out, fmul, 5);

    /* OpCompositeExtract float id_pos_x = id_load_pos[0] */
    uint32_t ext_x[] = {
        spv_op(SpvOpCompositeExtract, 5),
        m->float_type,
        id_pos_x,
        id_load_pos,
        0u          /* component 0 = x */
    };
    spvbuf_push_n(out, ext_x, 5);

    /* OpFAdd float id_new_x = id_pos_x + id_delta */
    uint32_t fadd[] = {
        spv_op(SpvOpFAdd, 5),
        m->float_type,
        id_new_x,
        id_pos_x,
        id_delta
    };
    spvbuf_push_n(out, fadd, 5);

    /* OpCompositeInsert vec4 id_new_pos = id_load_pos with [0]=id_new_x */
    uint32_t ins_x[] = {
        spv_op(SpvOpCompositeInsert, 6),
        m->v4_type,
        id_new_pos,
        id_new_x,
        id_load_pos,
        0u
    };
    spvbuf_push_n(out, ins_x, 6);

    /* OpStore *pos_ptr = id_new_pos  (via chain for block, direct for path A) */
    uint32_t store_pos[] = {
        spv_op(SpvOpStore, 3),
        pos_ptr,
        id_new_pos
    };
    spvbuf_push_n(out, store_pos, 3);

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
    float            convergence)
{
    (void)convergence; /* incorporated into left/right offsets by caller */

    if (!in_words || in_count < 5)
        return false;
    if (in_words[0] != SPIRV_MAGIC)
        return false;

    SpvModule m;
    memset(&m, 0, sizeof(m));
    m.words = in_words;
    m.count = in_count;
    spirv_scan(&m);

    if (!m.is_vertex) {
        STEREO_LOG("Shader scan: is_vertex=0, skipping patch");
        return false;
    }
    /* Log full scan results to diagnose gl_Position detection failures */
    STEREO_LOG("Shader scan: is_vertex=%d is_block=%d block_type=%u ptr_type=%u "
               "pos_var=%u member=%u float_type=%u v4=%u int_type=%u view_var=%u "
               "ptr_out_v4=%u ptr_in_int=%u bound=%u words=%zu",
               (int)m.is_vertex, (int)m.pos_is_block,
               m.pos_block_type, m.pos_ptr_type, m.pos_var, m.pos_member_idx,
               m.float_type, m.v4_type, m.int_type, m.view_var,
               m.ptr_out_v4_type, m.ptr_in_int_type, m.bound, (size_t)m.count);
    if (!m.pos_var) {
        STEREO_LOG("Shader scan: gl_Position NOT found "
                   "(block_type=%u ptr_type=%u) — skipping patch",
                   m.pos_block_type, m.pos_ptr_type);
        return false;
    }

    STEREO_LOG("Patching vertex shader: pos_var=%u view_var=%u bound=%u",
               m.pos_var, m.view_var, m.bound);

    /* ── Build two buffers: type-decl extras and function-body extras ── */
    SpvBuf type_extras, body_extras;
    if (!spvbuf_init(&type_extras, 64) || !spvbuf_init(&body_extras, 64)) {
        spvbuf_free(&type_extras);
        spvbuf_free(&body_extras);
        return false;
    }

    uint32_t next_id = m.bound;

    /* inject_stereo_code pushes type decls then body ops into the same buf.
     * We'll separate them by checking our known split point. */
    SpvBuf combined;
    if (!spvbuf_init(&combined, 256)) {
        spvbuf_free(&type_extras);
        spvbuf_free(&body_extras);
        return false;
    }

    if (!inject_stereo_code(&combined, &m, &next_id, left_offset, right_offset)) {
        STEREO_LOG("Stereo code injection skipped (missing types)");
        spvbuf_free(&combined);
        spvbuf_free(&type_extras);
        spvbuf_free(&body_extras);
        return false;
    }

    /* For simplicity, split at STEREO_TYPE_SPLIT: the first SpvOpLoad or
     * function body instruction.  Heuristic: type decls use small opcodes. */
    size_t type_split = 0;
    for (size_t i = 0; i < combined.count; ) {
        uint32_t op = combined.words[i] & 0xffff;
        if (op == SpvOpLoad || op == SpvOpStore ||
            op == SpvOpIEqual || op == SpvOpSelect ||
            op == SpvOpFMul  || op == SpvOpFAdd   ||
            op == SpvOpCompositeExtract || op == SpvOpCompositeInsert) {
            type_split = i;
            break;
        }
        uint32_t wc = combined.words[i] >> 16;
        if (!wc) break;
        i += wc;
        type_split = i;
    }

    /* type_extras = combined[0..type_split)
     * body_extras = combined[type_split..) */
    for (size_t i = 0; i < type_split; i++)
        spvbuf_push(&type_extras, combined.words[i]);
    for (size_t i = type_split; i < combined.count; i++)
        spvbuf_push(&body_extras, combined.words[i]);
    spvbuf_free(&combined);

    /* ── Two-pass rebuild ────────────────────────────────────────────── */
    /* Pass 1: find insertion points in the original stream.
     *   - Type section end: just before first OpFunction
     *   - Body insertion point:
     *     Path A (direct var): after last OpStore to pos_var
     *     Path B (block member): before first OpReturn (AccessChain is emitted
     *       by us; there is no raw OpStore to pos_var in the original stream) */
    size_t insert_type_after = 0; /* word index to insert type extras after */
    size_t insert_body_after = 0; /* word index to insert body extras after */
    {
        size_t i = 5;
        while (i < in_count) {
            uint32_t op     = in_words[i] & 0xffff;
            uint32_t wcount = in_words[i] >> 16;
            if (!wcount || i + wcount > in_count) break;

            if (op == SpvOpFunction && insert_type_after == 0)
                insert_type_after = i; /* insert type decls here */

            /* Path A only: track the last store directly to pos_var */
            if (!m.pos_is_block &&
                op == SpvOpStore && wcount >= 3 && in_words[i+1] == m.pos_var)
                insert_body_after = i + wcount; /* last OpStore to pos_var */

            i += wcount;
        }
    }

    if (insert_type_after == 0) {
        STEREO_LOG("No OpFunction found in vertex shader — skipping patch");
        spvbuf_free(&type_extras);
        spvbuf_free(&body_extras);
        return false;
    }

    if (insert_body_after == 0) {
        /* Path B (block member) or path A with no direct store found:
         * inject our code immediately before the first OpReturn so it runs
         * after the shader has written its final gl_Position value. */
        if (m.pos_is_block)
            STEREO_LOG("Block-member Position: injecting before first OpReturn");
        else
            STEREO_LOG("No direct OpStore to gl_Position; injecting before first OpReturn");
        /* Find first OpReturn */
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
            /* No OpReturn found — cannot safely inject; skip this shader. */
            STEREO_LOG("No OpReturn found in vertex shader — skipping patch");
            spvbuf_free(&type_extras);
            spvbuf_free(&body_extras);
            return false;
        }
    }

    /* Sanity: body injection must come after type section */
    if (insert_body_after < insert_type_after) {
        STEREO_LOG("insert_body_after(%zu) < insert_type_after(%zu) — skipping patch",
                   insert_body_after, insert_type_after);
        spvbuf_free(&type_extras);
        spvbuf_free(&body_extras);
        return false;
    }

    /* Pass 2: Copy original + splice in extras
     *
     * IMPORTANT: The SPIR-V header is 5 raw words (magic, version, generator,
     * bound, schema) -- they are NOT instructions and have no wcount/opcode
     * encoding.  The instruction loop must start at word index 5.  Starting
     * at 0 interprets the magic number 0x07230203 as an instruction whose
     * wcount = 0x0723 = 1827, which immediately fails the bounds check and
     * breaks with 0 words output.
     */
    SpvBuf out;
    if (!spvbuf_init(&out, in_count + type_extras.count + body_extras.count + 16)) {
        spvbuf_free(&type_extras);
        spvbuf_free(&body_extras);
        return false;
    }

    /* Copy the 5-word header first and update the ID bound word */
    spvbuf_push_n(&out, in_words, 5);
    out.words[3] = next_id;

    /* Walk instructions starting at word 5 (skip the header) */
    for (size_t i = 5; i < in_count; ) {
        /* Inject type extras just before first OpFunction */
        if (i == insert_type_after && type_extras.count > 0) {
            spvbuf_push_n(&out, type_extras.words, type_extras.count);
            type_extras.count = 0;
        }
        /* Inject body extras at target position */
        if (i == insert_body_after && body_extras.count > 0) {
            spvbuf_push_n(&out, body_extras.words, body_extras.count);
            body_extras.count = 0;
        }

        uint32_t wcount = in_words[i] >> 16;
        if (!wcount || i + wcount > in_count) break;

        spvbuf_push_n(&out, &in_words[i], wcount);
        i += wcount;
    }

    /* Append any extras whose insertion point was == in_count */
    if (type_extras.count > 0)
        spvbuf_push_n(&out, type_extras.words, type_extras.count);
    if (body_extras.count > 0)
        spvbuf_push_n(&out, body_extras.words, body_extras.count);

    spvbuf_free(&type_extras);
    spvbuf_free(&body_extras);

    *out_words = out.words;
    *out_count = out.count;

    STEREO_LOG("SPIR-V patched: %zu -> %zu words, new bound=%u",
               in_count, out.count, next_id);
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

    /* VKS3D_NO_SHADER_PATCH=1 disables SPIR-V patching for debugging.
     * Use this to test whether a crash is caused by our patched SPIR-V. */
    static int no_patch = -1;
    if (no_patch < 0) {
        const char *e = stereo_getenv("VKS3D_NO_SHADER_PATCH");
        no_patch = (e && e[0] == '1') ? 1 : 0;
        if (no_patch) STEREO_LOG("VKS3D_NO_SHADER_PATCH=1: SPIR-V patching disabled");
    }

    if (!sd->stereo.enabled || no_patch) {
        STEREO_LOG("stereo_CreateShaderModule: passthrough (%zu bytes)",
                   pCreateInfo ? pCreateInfo->codeSize : 0);
        VkResult r = sd->real.CreateShaderModule(sd->real_device, pCreateInfo, pAllocator, pShaderModule);
        STEREO_LOG("stereo_CreateShaderModule: passthrough result=%d module=%p", r,
                   pShaderModule ? (void*)(uintptr_t)*pShaderModule : NULL);
        return r;
    }

    /* Try to patch the SPIR-V */
    const uint32_t *in  = (const uint32_t*)pCreateInfo->pCode;
    size_t          in_c = pCreateInfo->codeSize / 4;
    uint32_t       *patched = NULL;
    size_t          patched_c = 0;

    bool patched_ok = spirv_patch_stereo_vertex(
        in, in_c, &patched, &patched_c,
        sd->stereo.left_eye_offset,
        sd->stereo.right_eye_offset,
        sd->stereo.convergence);

    VkShaderModuleCreateInfo mod_ci = *pCreateInfo;
    if (patched_ok) {
        mod_ci.pCode    = patched;
        mod_ci.codeSize = patched_c * sizeof(uint32_t);
        STEREO_LOG("stereo_CreateShaderModule: submitting patched SPIR-V (%zu words) to driver",
                   patched_c);
    } else {
        STEREO_LOG("stereo_CreateShaderModule: submitting original SPIR-V (%zu words) to driver",
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

    if (patched_ok)
        spirv_patched_free(patched);

    return res;
}

/* ── vkCreateGraphicsPipelines — logged to identify crash point ─────────── */
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

/* ── vkDestroyShaderModule ────────────────────────────────────────────────── */
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
