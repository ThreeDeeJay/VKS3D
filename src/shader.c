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
    uint32_t        pos_var;   /* ID of gl_Position variable          */
    uint32_t        view_var;  /* ID of gl_ViewIndex variable         */
    uint32_t        float_type;/* ID of OpTypeFloat 32                */
    uint32_t        v4_type;   /* ID of OpTypeVector float 4          */
    uint32_t        int_type;  /* ID of OpTypeInt 32 0 (unsigned)     */
    uint32_t        bool_type; /* ID of OpTypeBool                    */
    /* Positions of key sections in word stream */
    size_t          first_function_word; /* start of first OpFunction  */
    size_t          header_end;          /* end of type/decoration section */
} SpvModule;

static void spirv_scan(SpvModule *m)
{
    const uint32_t *w = m->words;
    size_t          i = 5; /* skip header */
    m->bound = w[3];

    while (i < m->count) {
        uint32_t op_code = w[i] & 0xffff;
        uint32_t wcount  = w[i] >> 16;
        if (wcount == 0 || i + wcount > m->count) break;

        switch (op_code) {
        case SpvOpEntryPoint:
            /* w[i+1]=ExecutionModel, w[i+2]=id, then name string */
            if (wcount >= 2 && w[i+1] == SpvExecutionModelVertex)
                m->is_vertex = true;
            break;
        case SpvOpTypeFloat:
            /* w[i+1]=result_id, w[i+2]=width */
            if (wcount == 3 && w[i+2] == 32)
                m->float_type = w[i+1];
            break;
        case SpvOpTypeVector:
            /* w[i+1]=result_id, w[i+2]=component_type, w[i+3]=count */
            if (wcount == 4 && w[i+2] == m->float_type && w[i+3] == 4)
                m->v4_type = w[i+1];
            break;
        case SpvOpTypeInt:
            /* w[i+1]=result_id, w[i+2]=width, w[i+3]=signedness */
            if (wcount == 4 && w[i+2] == 32)
                m->int_type = w[i+1];
            break;
        case SpvOpTypeBool:
            if (wcount == 2)
                m->bool_type = w[i+1];
            break;
        case SpvOpDecorate:
            /* w[i+1]=target, w[i+2]=Decoration, w[i+3...]=values */
            if (wcount >= 4 &&
                w[i+2] == SpvDecorationBuiltIn &&
                w[i+3] == SpvBuiltInPosition)
            {
                m->pos_var = w[i+1];   /* variable decorated as Position */
            }
            if (wcount >= 4 &&
                w[i+2] == SpvDecorationBuiltIn &&
                w[i+3] == SpvBuiltInViewIndex)
            {
                m->view_var = w[i+1];
            }
            break;
        case SpvOpFunction:
            if (m->first_function_word == 0)
                m->first_function_word = i;
            break;
        default:
            break;
        }
        i += wcount;
    }
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

    /* Allocate IDs */
    uint32_t id_ptr_v4_out = (*next_id)++;   /* OpTypePointer Output v4  */
    uint32_t id_ptr_v4_in  = (*next_id)++;   /* OpTypePointer Input  v4  (unused but allocated) */
    uint32_t id_ptr_int_in = (*next_id)++;   /* OpTypePointer Input  int */
    uint32_t id_load_pos   = (*next_id)++;   /* loaded gl_Position       */
    uint32_t id_load_view  = (*next_id)++;   /* loaded gl_ViewIndex      */
    uint32_t id_const_zero_i = (*next_id)++; /* constant int 0           */
    uint32_t id_is_left    = (*next_id)++;   /* bool: viewIndex == 0     */
    uint32_t id_const_l    = (*next_id)++;   /* constant float left_off  */
    uint32_t id_const_r    = (*next_id)++;   /* constant float right_off */
    uint32_t id_off_sel    = (*next_id)++;   /* selected offset          */
    uint32_t id_pos_w      = (*next_id)++;   /* pos.w (component 3)      */
    uint32_t id_delta      = (*next_id)++;   /* off * pos.w              */
    uint32_t id_pos_x      = (*next_id)++;   /* pos.x (component 0)      */
    uint32_t id_new_x      = (*next_id)++;   /* pos.x + delta            */
    uint32_t id_new_pos    = (*next_id)++;   /* composite with new x     */

    /* We'll need a pointer to gl_Position (Output vec4) */
    /* and a pointer to gl_ViewIndex (Input int if available) */

    (void)id_ptr_v4_in;   /* may not need */
    (void)id_ptr_int_in;  /* may not need */

    /* ── Emit type declarations for new IDs ──────────────────────────
     * These must go in the type/decoration section BEFORE functions.
     * We insert them during the second-pass copy (handled by caller).
     * Here we just build the instruction words. */

    /* OpTypePointer Output v4  — for gl_Position */
    uint32_t ptr_out_v4_words[] = {
        spv_op(SpvOpTypePointer, 4),
        id_ptr_v4_out,
        SpvStorageClassOutput,
        m->v4_type
    };

    /* OpTypePointer Input int  — for gl_ViewIndex */
    uint32_t int_type = m->int_type;
    if (!int_type) {
        /* Need to allocate int type too */
        /* This is handled as a type declaration */
    }
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

    /* Emit type declarations (will be placed before first function by caller) */
    spvbuf_push_n(out, ptr_out_v4_words, 4);
    if (int_type) {
        spvbuf_push_n(out, ptr_in_int_words, 4);
        spvbuf_push_n(out, const_i0_words, 4);
    }
    spvbuf_push_n(out, const_l_words, 4);
    spvbuf_push_n(out, const_r_words, 4);

    /* ── Function body instructions (go after the last OpStore pos_var) ── */
    /* OpLoad vec4 id_load_pos = *pos_var */
    uint32_t load_pos[] = { spv_op(SpvOpLoad, 4), m->v4_type, id_load_pos, m->pos_var };
    spvbuf_push_n(out, load_pos, 4);

    /* If we have view_var, load it; else default to left offset always */
    if (m->view_var && int_type) {
        /* OpLoad int id_load_view = *view_var */
        uint32_t load_view[] = { spv_op(SpvOpLoad, 4), int_type, id_load_view, m->view_var };
        spvbuf_push_n(out, load_view, 4);

        /* OpIEqual bool id_is_left = id_load_view == const_zero_i */
        uint32_t is_left[] = {
            spv_op(SpvOpIEqual, 5),
            m->bool_type ? m->bool_type : (*next_id)++,
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

    /* OpStore *pos_var = id_new_pos */
    uint32_t store_pos[] = {
        spv_op(SpvOpStore, 3),
        m->pos_var,
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
        STEREO_LOG("Shader: not vertex, skipping patch");
        return false;
    }
    if (!m.pos_var) {
        STEREO_LOG("Shader: no gl_Position found, skipping patch");
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
     *   - Body insertion point: after last OpStore to pos_var in each function */
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

            if (op == SpvOpStore && wcount >= 3 && in_words[i+1] == m.pos_var)
                insert_body_after = i + wcount; /* last OpStore to pos_var */

            i += wcount;
        }
    }

    if (insert_type_after == 0)
        insert_type_after = in_count; /* append at end */

    if (insert_body_after == 0) {
        STEREO_LOG("No OpStore to gl_Position found; injecting before first OpReturn");
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
        if (!insert_body_after)
            insert_body_after = in_count;
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

    if (!sd->stereo.enabled) {
        return sd->real.CreateShaderModule(sd->real_device, pCreateInfo, pAllocator, pShaderModule);
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
    }

    VkResult res = sd->real.CreateShaderModule(
        sd->real_device, &mod_ci, pAllocator, pShaderModule);

    if (patched_ok)
        spirv_patched_free(patched);

    return res;
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
