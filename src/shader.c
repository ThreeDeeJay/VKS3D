/*
 * shader.c — SPIR-V stereo injection, deferred to pipeline creation
 *
 * Architecture
 * ─────────────
 * Patching VS at CreateShaderModule time is wrong when a GS follows: the GS
 * recomputes gl_Position from scratch, silently discarding the VS offset.
 *
 * Fix: cache original SPIR-V at CreateShaderModule; at CreateGraphicsPipelines
 * inspect the stage list, select the last pre-rast stage (GS > TES > VS), and
 * patch only that stage's SPIR-V with a temporary VkShaderModule.
 *
 * GS injection point (key fix vs. prior iteration)
 * ─────────────────────────────────────────────────
 * Injecting before OpReturn is WRONG for GS: OpEmitVertex already fired and
 * consumed gl_Position.  Instead we inject a fresh offset-block (with unique
 * SSA IDs) immediately before EVERY OpEmitVertex instruction.  Each block:
 *   1. OpAccessChain / direct ptr  → pointer to gl_Position output
 *   2. OpLoad                      → current gl_Position
 *   3. OpLoad gl_ViewIndex         → view index (0=left, 1=right)
 *   4. OpIEqual + OpSelect         → pick left or right float offset
 *   5. OpCompositeExtract .w       → perspective divisor
 *   6. OpFMul                      → delta = offset * w
 *   7. OpCompositeExtract .x       → current x
 *   8. OpFAdd                      → new x
 *   9. OpCompositeInsert           → patched position
 *  10. OpStore                     → write back before emit
 *
 * VS/TES injection is a single instance of the same block, inserted either
 * after the last OpStore to pos_var (path A) or before OpReturn (path B).
 *
 * Stage priority:  GS (prio 2) > TES (prio 1) > VS (prio 0)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include "stereo_icd.h"

/* ── SPIR-V opcodes ───────────────────────────────────────────────────────── */
#define SpvOpCapability     17
#define SpvOpEntryPoint     15
#define SpvOpTypeBool       20
#define SpvOpTypeInt        21
#define SpvOpTypeFloat      22
#define SpvOpTypeVector     23
#define SpvOpTypePointer    32
#define SpvOpConstant       43
#define SpvOpVariable       59
#define SpvOpLoad           61
#define SpvOpStore          62
#define SpvOpAccessChain    65
#define SpvOpDecorate       71
#define SpvOpMemberDecorate 72
#define SpvOpFunction       54
#define SpvOpEmitVertex     218
#define SpvOpCompositeExtract 81
#define SpvOpCompositeInsert  82
#define SpvOpFAdd           129
#define SpvOpFMul           133
#define SpvOpIEqual         170
#define SpvOpSelect         169

#define SpvDecorationBuiltIn  11
#define SpvBuiltInPosition     0
#define SpvBuiltInViewIndex 4440

#define SpvStorageInput   1
#define SpvStorageOutput  3

#define SpvExecVertex   0
#define SpvExecTessEval 2
#define SpvExecGeometry 3

#define SpvOpTypeArray    28
#define SpvCapPVNA       5260   /* PerViewAttributesNV — write both eye positions at once */
#define SpvBuiltInPVNV   5261   /* PositionPerViewNV builtin */

#define SpvCapMV 5296   /* MultiView */

#define SPIRV_MAGIC 0x07230203u

/* ── Dynamic word buffer ──────────────────────────────────────────────────── */
typedef struct { uint32_t *w; size_t n, cap; } SpvBuf;
static bool  sb_init(SpvBuf *b, size_t c) { b->w=malloc(c*4); b->n=0; b->cap=c; return !!b->w; }
static void  sb_free(SpvBuf *b)           { free(b->w); b->w=NULL; b->n=b->cap=0; }
static bool  sb_push(SpvBuf *b, uint32_t v) {
    if (b->n>=b->cap) { uint32_t *p=realloc(b->w,b->cap*8); if(!p)return false; b->w=p; b->cap*=2; }
    b->w[b->n++]=v; return true; }
static bool  sb_push_n(SpvBuf *b, const uint32_t *v, size_t c) {
    for(size_t i=0;i<c;i++) if(!sb_push(b,v[i])) return false; return true; }
static inline uint32_t op_(uint32_t op, uint32_t wc) { return (wc<<16)|op; }

/* ── Module info ──────────────────────────────────────────────────────────── */
typedef struct {
    const uint32_t *words; size_t count;
    uint32_t  bound;
    bool      is_patchable, has_mv_cap;
    int       exec_model;          /* SpvExecVertex / TessEval / Geometry */

    /* gl_Position */
    uint32_t  pos_var;             /* direct variable (path A)            */
    bool      pos_is_block;        /* block member (path B)               */
    uint32_t  pos_block_type, pos_member_idx, pos_ptr_type;

    /* supporting types */
    uint32_t  view_var;
    uint32_t  ft, v4t, it, bt;    /* float32, vec4, int32, bool types    */
    uint32_t  ptr_out_v4;          /* existing OpTypePointer Output vec4  */
    uint32_t  ptr_in_int;          /* existing OpTypePointer Input  int   */
    size_t    fn_word;             /* offset of first OpFunction          */
    uint32_t  emit_count;          /* number of OpEmitVertex in body (GS) */
} SpvMod;

static void do_scan(SpvMod *m, bool p2)
{
    const uint32_t *w=m->words;
    for (size_t i=5; i<m->count; ) {
        uint32_t op=w[i]&0xffff, wc=w[i]>>16;
        if (!wc||i+wc>m->count) break;
        if (!p2) switch(op) {
        case SpvOpCapability:     if(wc>=2&&w[i+1]==SpvCapMV) m->has_mv_cap=true; break;
        case SpvOpEntryPoint:
            if(wc>=2){uint32_t e=w[i+1];
                if(e==SpvExecVertex||e==SpvExecTessEval||e==SpvExecGeometry)
                    {m->is_patchable=true;m->exec_model=(int)e;}} break;
        case SpvOpTypeFloat:      if(wc==3&&w[i+2]==32) m->ft=w[i+1];  break;
        case SpvOpTypeVector:     if(wc==4&&w[i+2]==m->ft&&w[i+3]==4) m->v4t=w[i+1]; break;
        case SpvOpTypeInt:        if(wc==4&&w[i+2]==32) m->it=w[i+1];  break;
        case SpvOpTypeBool:       if(wc==2) m->bt=w[i+1];              break;
        case SpvOpTypePointer:
            if(wc>=4){
                if(w[i+2]==SpvStorageOutput&&m->v4t&&w[i+3]==m->v4t) m->ptr_out_v4=w[i+1];
                if(w[i+2]==SpvStorageInput &&m->it  &&w[i+3]==m->it ) m->ptr_in_int=w[i+1];
            } break;
        case SpvOpDecorate:
            if(wc>=4&&w[i+2]==SpvDecorationBuiltIn){
                if(w[i+3]==SpvBuiltInPosition&&!m->pos_is_block) m->pos_var=w[i+1];
                if(w[i+3]==SpvBuiltInViewIndex)                  m->view_var=w[i+1];
            } break;
        case SpvOpMemberDecorate:
            if(wc>=5&&w[i+3]==SpvDecorationBuiltIn&&w[i+4]==SpvBuiltInPosition)
                {m->pos_block_type=w[i+1];m->pos_member_idx=w[i+2];m->pos_is_block=true;m->pos_var=0;} break;
        case SpvOpFunction:       if(!m->fn_word) m->fn_word=i; break;
        case SpvOpEmitVertex:     m->emit_count++; break;
        } else {
            if(op==SpvOpTypePointer&&wc>=4&&w[i+2]==SpvStorageOutput&&m->pos_block_type&&w[i+3]==m->pos_block_type) m->pos_ptr_type=w[i+1];
            if(op==SpvOpVariable &&wc>=4&&w[i+3]==SpvStorageOutput&&m->pos_ptr_type&&w[i+1]==m->pos_ptr_type) m->pos_var=w[i+2];
        }
        i+=wc;
    }
}
static void spv_scan(SpvMod *m)
{ m->bound=m->words[3]; do_scan(m,false); if(m->pos_is_block) do_scan(m,true); }

/* ── Shared state for body emission ──────────────────────────────────────── */
typedef struct {
    SpvMod  *m;
    bool     have_view;   /* gl_ViewIndex available (existing or injected) */
    uint32_t uv4;         /* ptr type: Output vec4   (shared across insts) */
    uint32_t uint_;       /* ptr type: Input  int    (shared)               */
    uint32_t bt;          /* bool type               (shared)               */
    uint32_t cz;          /* const int  0            (shared)               */
    uint32_t cl, cr;      /* const float left/right  (shared)               */
    /* PVNA mode: write both eye positions to gl_PositionPerViewNV[] instead
     * of reading gl_ViewIndex.  Used for VS/GS where driver 426.06 does not
     * populate gl_ViewIndex, but does honour per-view attribute outputs.    */
    bool     pvna;        /* true → use PositionPerViewNV, false → ViewIndex */
    uint32_t pvnv_var;    /* gl_PositionPerViewNV Output variable ID         */
    uint32_t c1;          /* const int 1 (for pvnv[1] access)                */
} BodyCtx;

/* Emit one instance of the stereo-offset body.
 * All SSA result IDs are freshly allocated from *nid to avoid duplicates
 * when called multiple times (once per OpEmitVertex in a GS). */
static void emit_body(SpvBuf *out, const BodyCtx *c, uint32_t *nid)
{
    SpvMod *m = c->m;

    /* Allocate per-instance IDs (shared by both paths) */
    uint32_t ch   = (*nid)++;   /* AccessChain result (block path only) */
    uint32_t lp   = (*nid)++;   /* loaded gl_Position                   */

    /* Pointer to gl_Position */
    uint32_t pptr;
    if (m->pos_is_block) {
        uint32_t member_idx_id;
        if (m->pos_member_idx == 0) {
            member_idx_id = c->cz;
        } else {
            member_idx_id = (*nid)++;
            uint32_t ci[]={op_(SpvOpConstant,4),m->it,member_idx_id,m->pos_member_idx};
            sb_push_n(out,ci,4);
        }
        uint32_t a[]={op_(SpvOpAccessChain,5),c->uv4,ch,m->pos_var,member_idx_id};
        sb_push_n(out,a,5); pptr=ch;
    } else { pptr=m->pos_var; }

    /* Load gl_Position */
    { uint32_t w[]={op_(SpvOpLoad,4),m->v4t,lp,pptr}; sb_push_n(out,w,4); }

    if (c->pvna && c->pvnv_var) {
        /* ── PVNA path: compute both eye positions, write to pvnv[0]/[1] ──── *
         * The driver picks pvnv[view_index] for each view automatically.      *
         * No need for gl_ViewIndex at all — works on NVIDIA 426.06 VS/GS.    */
        uint32_t pw   = (*nid)++;   /* pos.w                             */
        uint32_t lox  = (*nid)++;   /* left  delta x = left_offset  * w  */
        uint32_t rox  = (*nid)++;   /* right delta x = right_offset * w  */
        uint32_t px   = (*nid)++;   /* pos.x                             */
        uint32_t lx   = (*nid)++;   /* left  eye x                       */
        uint32_t rx   = (*nid)++;   /* right eye x                       */
        uint32_t lpos = (*nid)++;   /* full left  eye vec4               */
        uint32_t rpos = (*nid)++;   /* full right eye vec4               */
        uint32_t p0   = (*nid)++;   /* &pvnv[0]                          */
        uint32_t p1   = (*nid)++;   /* &pvnv[1]                          */

        { uint32_t w[]={op_(SpvOpCompositeExtract,5),m->ft,pw,lp,3u};   sb_push_n(out,w,5); }
        { uint32_t w[]={op_(SpvOpFMul,5),m->ft,lox,c->cl,pw};           sb_push_n(out,w,5); }
        { uint32_t w[]={op_(SpvOpFMul,5),m->ft,rox,c->cr,pw};           sb_push_n(out,w,5); }
        { uint32_t w[]={op_(SpvOpCompositeExtract,5),m->ft,px,lp,0u};   sb_push_n(out,w,5); }
        { uint32_t w[]={op_(SpvOpFAdd,5),m->ft,lx,px,lox};              sb_push_n(out,w,5); }
        { uint32_t w[]={op_(SpvOpFAdd,5),m->ft,rx,px,rox};              sb_push_n(out,w,5); }
        { uint32_t w[]={op_(SpvOpCompositeInsert,6),m->v4t,lpos,lx,lp,0u}; sb_push_n(out,w,6); }
        { uint32_t w[]={op_(SpvOpCompositeInsert,6),m->v4t,rpos,rx,lp,0u}; sb_push_n(out,w,6); }
        /* pvnv[0] = left eye position */
        { uint32_t w[]={op_(SpvOpAccessChain,5),c->uv4,p0,c->pvnv_var,c->cz}; sb_push_n(out,w,5); }
        { uint32_t w[]={op_(SpvOpStore,3),p0,lpos};                     sb_push_n(out,w,3); }
        /* pvnv[1] = right eye position */
        { uint32_t w[]={op_(SpvOpAccessChain,5),c->uv4,p1,c->pvnv_var,c->c1}; sb_push_n(out,w,5); }
        { uint32_t w[]={op_(SpvOpStore,3),p1,rpos};                     sb_push_n(out,w,3); }

    } else {
        /* ── gl_ViewIndex path (TES, or any stage where it works) ─────────── */
        uint32_t lv   = c->have_view ? (*nid)++ : 0;
        uint32_t isl  = c->have_view ? (*nid)++ : 0;
        uint32_t sel  = (*nid)++;
        uint32_t pw   = (*nid)++;
        uint32_t dlt  = (*nid)++;
        uint32_t px   = (*nid)++;
        uint32_t nx   = (*nid)++;
        uint32_t np   = (*nid)++;

        if (c->have_view && m->view_var && m->it && c->bt) {
            { uint32_t w[]={op_(SpvOpLoad,4),m->it,lv,m->view_var};         sb_push_n(out,w,4); }
            { uint32_t w[]={op_(SpvOpIEqual,5),c->bt,isl,lv,c->cz};        sb_push_n(out,w,5); }
            { uint32_t w[]={op_(SpvOpSelect,6),m->ft,sel,isl,c->cl,c->cr}; sb_push_n(out,w,6); }
        } else {
            STEREO_LOG("emit_body: no view index (have_view=%d view_var=%u it=%u bt=%u) — using constant left offset",
                       (int)c->have_view, m->view_var, m->it, (unsigned)(uintptr_t)c->bt);
            sel=c->cl;
        }

        { uint32_t w[]={op_(SpvOpCompositeExtract,5),m->ft,pw,lp,3u};      sb_push_n(out,w,5); }
        { uint32_t w[]={op_(SpvOpFMul,5),m->ft,dlt,sel,pw};                sb_push_n(out,w,5); }
        { uint32_t w[]={op_(SpvOpCompositeExtract,5),m->ft,px,lp,0u};      sb_push_n(out,w,5); }
        { uint32_t w[]={op_(SpvOpFAdd,5),m->ft,nx,px,dlt};                 sb_push_n(out,w,5); }
        { uint32_t w[]={op_(SpvOpCompositeInsert,6),m->v4t,np,nx,lp,0u};   sb_push_n(out,w,6); }
        { uint32_t w[]={op_(SpvOpStore,3),pptr,np};                        sb_push_n(out,w,3); }
    }
}

/* ── Patcher ──────────────────────────────────────────────────────────────── */
bool spirv_patch_stereo_vertex(
    const uint32_t *in, size_t in_c,
    uint32_t **out, size_t *out_c,
    float lo, float ro, float conv, bool inj_vi)
{
    (void)conv;
    if (!in||in_c<5||in[0]!=SPIRV_MAGIC) {
        STEREO_LOG("Shader: bad header in=%p in_c=%zu magic=0x%x (expected 0x%x)",
                   (void*)in, in_c, in ? in[0] : 0u, SPIRV_MAGIC);
        return false;
    }

    SpvMod m={0}; m.words=in; m.count=in_c;
    spv_scan(&m);

    if (!m.is_patchable) { STEREO_LOG("Shader: not patchable (model=%d)",m.exec_model); return false; }
    if (!m.pos_var)       { STEREO_LOG("Shader: gl_Position not found");                return false; }
    STEREO_LOG("Shader patch: model=%d block=%d pos_var=%u view_var=%u emit_count=%u bound=%u",
               m.exec_model,(int)m.pos_is_block,m.pos_var,m.view_var,m.emit_count,m.bound);

    bool is_gs = (m.exec_model == SpvExecGeometry);

    /* PVNA (PerViewAttributesNV): VS and GS on NVIDIA 426.06 do not populate
     * gl_ViewIndex during multiview rendering.  Instead, write both eye
     * positions to gl_PositionPerViewNV[0/1] in a single invocation and let
     * the driver distribute them per-view.  TES keeps gl_ViewIndex (works). */
    bool use_pvna = (m.exec_model == SpvExecVertex || m.exec_model == SpvExecGeometry);

    /* ── Allocate shared IDs ─────────────────────────────────────────────── */
    uint32_t nid = m.bound;

    /* Optional: new ptr types if not already declared */
    uint32_t id_ptr_v4  = nid++;   /* OpTypePointer Output vec4 (if needed) */
    uint32_t id_ptr_int = nid++;   /* OpTypePointer Input  int  (if needed) */

    /* Bug fix: simple VS shaders may have no integer type at all (no int ops).
     * Without m.it we cannot declare gl_ViewIndex (an int builtin) nor create
     * the comparison constants.  Create a signed int32 type when missing so
     * that view-index injection works even in the simplest MVP vertex shader. */
    uint32_t id_new_it = 0;
    if (!m.it && inj_vi && !m.view_var) {
        id_new_it = nid++;     /* will emit OpTypeInt 32 1 (signed) */
        m.it      = id_new_it; /* from this point m.it is usable    */
        STEREO_LOG("Shader: no int type found — creating id=%u for gl_ViewIndex", id_new_it);
    }

    /* PVNA-specific IDs (VS/GS only) */
    uint32_t id_uint_type  = 0; /* OpTypeInt 32 0 (unsigned, for array size)  */
    uint32_t id_const_2    = 0; /* OpConstant uint 2 (array length)           */
    uint32_t id_v4arr2     = 0; /* OpTypeArray vec4 2                         */
    uint32_t id_ptr_v4arr2 = 0; /* OpTypePointer Output (array)               */
    uint32_t id_pvnv_var   = 0; /* gl_PositionPerViewNV Output variable       */
    uint32_t id_const_1    = 0; /* OpConstant int 1 (for pvnv[1] index)       */
    if (use_pvna) {
        id_uint_type  = nid++;
        id_const_2    = nid++;
        id_v4arr2     = nid++;
        id_ptr_v4arr2 = nid++;
        id_pvnv_var   = nid++;
        id_const_1    = nid++;
    }

    /* gl_ViewIndex injection (TES only — not used in PVNA mode) */
    bool     will_inj_vi = !use_pvna && inj_vi && !m.view_var && m.it;
    uint32_t id_inj_view = will_inj_vi ? nid++ : 0;
    bool     have_view   = !use_pvna && (m.view_var || will_inj_vi);
    /* Optional: new bool type (for gl_ViewIndex IEqual result) */
    uint32_t id_new_bt = 0;
    if (!use_pvna && !m.bt && have_view && m.it) { id_new_bt=nid++; }

    /* Shared constants */
    uint32_t id_cz = nid++; /* const int  0 */
    uint32_t id_cl = nid++; /* const float left_offset  */
    uint32_t id_cr = nid++; /* const float right_offset */

    /* Resolved shared IDs */
    uint32_t uv4   = m.ptr_out_v4  ? m.ptr_out_v4  : id_ptr_v4;
    uint32_t uint_ = m.ptr_in_int   ? m.ptr_in_int   : id_ptr_int;
    uint32_t bt    = m.bt           ? m.bt           : id_new_bt;

    /* ── Build type_extras (emitted once before first OpFunction) ─────────── */
    SpvBuf te; if (!sb_init(&te,64)) return false;

    if (id_new_it) {
        /* OpTypeInt 32 1  (signed 32-bit integer, for gl_ViewIndex) */
        uint32_t w[]={op_(SpvOpTypeInt,4),id_new_it,32,1}; sb_push_n(&te,w,4); }
    if (!m.ptr_out_v4) {
        uint32_t w[]={op_(SpvOpTypePointer,4),id_ptr_v4,SpvStorageOutput,m.v4t};
        sb_push_n(&te,w,4); }
    if (m.it && !m.ptr_in_int) {
        uint32_t w[]={op_(SpvOpTypePointer,4),id_ptr_int,SpvStorageInput,m.it};
        sb_push_n(&te,w,4); m.ptr_in_int=id_ptr_int; uint_=id_ptr_int; }
    if (id_new_bt) { uint32_t w[]={op_(SpvOpTypeBool,2),id_new_bt}; sb_push_n(&te,w,2); }
    if (m.it) {
        uint32_t w[]={op_(SpvOpConstant,4),m.it,id_cz,0}; sb_push_n(&te,w,4); }
    { uint32_t w[4]={op_(SpvOpConstant,4),m.ft,id_cl,0}; memcpy(&w[3],&lo,4); sb_push_n(&te,w,4); }
    { uint32_t w[4]={op_(SpvOpConstant,4),m.ft,id_cr,0}; memcpy(&w[3],&ro,4); sb_push_n(&te,w,4); }

    /* PVNA declarations: array type + variable for gl_PositionPerViewNV */
    if (use_pvna) {
        /* unsigned int (for array length constant) */
        { uint32_t w[]={op_(SpvOpTypeInt,4),id_uint_type,32,0};            sb_push_n(&te,w,4); }
        /* const uint 2 (array length) */
        { uint32_t w[]={op_(SpvOpConstant,4),id_uint_type,id_const_2,2};  sb_push_n(&te,w,4); }
        /* OpTypeArray vec4 2 */
        { uint32_t w[]={op_(SpvOpTypeArray,4),id_v4arr2,m.v4t,id_const_2}; sb_push_n(&te,w,4); }
        /* OpTypePointer Output (OpTypeArray vec4 2) */
        { uint32_t w[]={op_(SpvOpTypePointer,4),id_ptr_v4arr2,SpvStorageOutput,id_v4arr2}; sb_push_n(&te,w,4); }
        /* OpDecorate pvnv_var BuiltIn PositionPerViewNV */
        { uint32_t w[]={op_(SpvOpDecorate,4),id_pvnv_var,SpvDecorationBuiltIn,SpvBuiltInPVNV}; sb_push_n(&te,w,4); }
        /* OpVariable (pointer) pvnv_var Output */
        { uint32_t w[]={op_(SpvOpVariable,4),id_ptr_v4arr2,id_pvnv_var,SpvStorageOutput}; sb_push_n(&te,w,4); }
        /* const int 1 (for pvnv[1] access index) */
        if (m.it) { uint32_t w[]={op_(SpvOpConstant,4),m.it,id_const_1,1}; sb_push_n(&te,w,4); }
    }

    /* Optional gl_ViewIndex variable injection (TES path, goes in type section) */
    uint32_t inj_view_id = 0;
    if (will_inj_vi) {
        uint32_t d[]={op_(SpvOpDecorate,4),id_inj_view,SpvDecorationBuiltIn,SpvBuiltInViewIndex};
        sb_push_n(&te,d,4);
        uint32_t v[]={op_(SpvOpVariable,4),uint_,id_inj_view,SpvStorageInput};
        sb_push_n(&te,v,4);
        m.view_var=id_inj_view; inj_view_id=id_inj_view;
    }

    /* ── Body context (shared data for emit_body) ─────────────────────────── */
    BodyCtx bc = { &m, have_view, uv4, uint_, bt, id_cz, id_cl, id_cr,
                   use_pvna, id_pvnv_var, id_const_1 };

    /* For GS we need emit_count * ~10 body IDs.  For VS/TES we need ~10. */
    uint32_t instances = is_gs ? (m.emit_count > 0 ? m.emit_count : 1) : 1;
    /* Reserve body ID space: ~12 IDs per instance should be plenty */
    (void)instances; /* nid is incremented inside emit_body per call */

    /* ── Find VS/TES single-body insertion point (not used for GS) ─────────── */
    size_t ins_t = 0; /* insert type_extras before first OpFunction  */
    size_t ins_b = 0; /* VS/TES: insert body before this word offset */
    for (size_t i=5; i<in_c; ) {
        uint32_t opx=in[i]&0xffff, wcx=in[i]>>16;
        if (!wcx||i+wcx>in_c) break;
        if (opx==SpvOpFunction && !ins_t) ins_t=i;
        /* Path A: inject after last OpStore to pos_var */
        if (!m.pos_is_block && opx==SpvOpStore && wcx>=3 && in[i+1]==m.pos_var)
            ins_b=i+wcx;
        i+=wcx;
    }
    if (!ins_t) { sb_free(&te); return false; }

    /* Path B fallback: inject before first OpReturn */
    if (!m.pos_is_block && ins_b==0 && !is_gs) {
        for (size_t i=5; i<in_c; ) {
            uint32_t opx=in[i]&0xffff, wcx=in[i]>>16;
            if (!wcx) break;
            if (opx==253||opx==254) { ins_b=i; break; }
            i+=wcx;
        }
    }
    if (m.pos_is_block && !is_gs) {
        /* Block path for VS/TES: inject before first OpReturn */
        for (size_t i=5; i<in_c; ) {
            uint32_t opx=in[i]&0xffff, wcx=in[i]>>16;
            if (!wcx) break;
            if (opx==253||opx==254) { ins_b=i; break; }
            i+=wcx;
        }
    }
    if (!is_gs && !ins_b) { sb_free(&te); return false; }
    if (!is_gs && ins_b < ins_t) { sb_free(&te); return false; }

    /* ── Pass 2: Rebuild output ───────────────────────────────────────────── */
    bool need_mv_cap  = inj_view_id && !m.has_mv_cap;
    bool need_pvna_cap = use_pvna;   /* always inject PerViewAttributesNV for VS/GS */
    bool mv_done      = false;
    bool pvna_done    = false;
    bool te_done      = false;
    bool vs_body_done = false;

    SpvBuf ob;
    /* Estimate output size: in_c + type_extras + bodies */
    size_t est = in_c + te.n + instances*32 + 32;
    if (!sb_init(&ob, est)) { sb_free(&te); return false; }
    sb_push_n(&ob, in, 5);
    ob.w[3] = nid + instances*12 + (use_pvna ? 16 : 0); /* upper bound; corrected below */

    for (size_t i=5; i<in_c; ) {
        /* Inject OpCapability MultiView before first instruction */
        if (!mv_done && need_mv_cap) {
            uint32_t c[]={op_(SpvOpCapability,2),SpvCapMV}; sb_push_n(&ob,c,2); mv_done=true; }
        /* Inject OpCapability PerViewAttributesNV for VS/GS */
        if (!pvna_done && need_pvna_cap) {
            uint32_t c[]={op_(SpvOpCapability,2),SpvCapPVNA}; sb_push_n(&ob,c,2); pvna_done=true; }

        /* Inject type_extras before first OpFunction */
        if (!te_done && i==ins_t) {
            sb_push_n(&ob,te.w,te.n); te_done=true; }

        uint32_t opx=in[i]&0xffff, wcx=in[i]>>16;
        if (!wcx||i+wcx>in_c) break;

        /* Extend OpEntryPoint interface for injected view var (TES path) */
        if (opx==SpvOpEntryPoint && wcx>=4 && inj_view_id &&
            (in[i+1]==SpvExecVertex||in[i+1]==SpvExecGeometry||in[i+1]==SpvExecTessEval)) {
            sb_push(&ob, ((wcx+1)<<16)|SpvOpEntryPoint);
            sb_push_n(&ob, &in[i+1], wcx-1);
            sb_push(&ob, inj_view_id);
            STEREO_LOG("OpEntryPoint extended: added view_var=%u", inj_view_id);
            i+=wcx; continue;
        }
        /* Extend OpEntryPoint interface for pvnv_var (PVNA path: VS/GS) */
        if (opx==SpvOpEntryPoint && wcx>=4 && use_pvna && id_pvnv_var &&
            (in[i+1]==SpvExecVertex||in[i+1]==SpvExecGeometry||in[i+1]==SpvExecTessEval)) {
            sb_push(&ob, ((wcx+1)<<16)|SpvOpEntryPoint);
            sb_push_n(&ob, &in[i+1], wcx-1);
            sb_push(&ob, id_pvnv_var);
            STEREO_LOG("OpEntryPoint extended: added pvnv_var=%u", id_pvnv_var);
            i+=wcx; continue;
        }

        /* GS: inject fresh body before each OpEmitVertex */
        if (is_gs && opx==SpvOpEmitVertex) {
            emit_body(&ob, &bc, &nid);
        }

        /* VS/TES: inject body at single insertion point */
        if (!is_gs && !vs_body_done && i==ins_b) {
            emit_body(&ob, &bc, &nid);
            vs_body_done=true;
        }

        sb_push_n(&ob, &in[i], wcx);
        i+=wcx;
    }
    /* Flush residuals */
    if (!te_done) sb_push_n(&ob,te.w,te.n);
    sb_free(&te);

    /* Correct the ID bound now that we know the final nid */
    ob.w[3] = nid;

    *out=ob.w; *out_c=ob.n;
    STEREO_LOG("Patched: model=%d  %zu→%zu words  bound=%u  vi=%d  pvna=%d  gs_inj=%u",
               m.exec_model, in_c, ob.n, nid, (int)(inj_view_id!=0),
               (int)use_pvna, is_gs?m.emit_count:1);
    return true;
}

void spirv_patched_free(uint32_t *w) { free(w); }

/* ── Quick scan: is this SPIR-V a patchable pre-rast stage? ─────────────── */
static bool is_patchable_spv(const uint32_t *w, size_t c)
{
    if (c<5||w[0]!=SPIRV_MAGIC) return false;
    for (size_t i=5; i<c; ) {
        uint32_t op=w[i]&0xffff, wc=w[i]>>16;
        if (!wc||i+wc>c) break;
        if (op==SpvOpEntryPoint&&wc>=2) {
            uint32_t e=w[i+1];
            return e==SpvExecVertex||e==SpvExecGeometry||e==SpvExecTessEval;
        }
        i+=wc;
    }
    return false;
}

/* ── Shader module cache ──────────────────────────────────────────────────── */
static StereoShaderCache *cache_find(StereoDevice *sd, VkShaderModule h)
{
    for (uint32_t i=0;i<sd->shader_cache_count;i++)
        if (sd->shader_cache[i].handle==h) return &sd->shader_cache[i];
    return NULL;
}
static void cache_add(StereoDevice *sd, VkShaderModule h,
                      const uint32_t *spv, size_t words)
{
    if (sd->shader_cache_count>=MAX_SHADER_CACHE) {
        STEREO_ERR("Shader cache full, dropping module %p",(void*)(uintptr_t)h); return; }
    uint32_t *cp=malloc(words*4); if (!cp) return;
    memcpy(cp,spv,words*4);
    StereoShaderCache *e=&sd->shader_cache[sd->shader_cache_count++];
    e->handle=h; e->spv=cp; e->words=words;
    STEREO_LOG("cache_add: %p  %zu words  slot=%u",
               (void*)(uintptr_t)h,words,sd->shader_cache_count-1);
}
static void cache_remove(StereoDevice *sd, VkShaderModule h)
{
    for (uint32_t i=0;i<sd->shader_cache_count;i++)
        if (sd->shader_cache[i].handle==h) {
            free(sd->shader_cache[i].spv);
            sd->shader_cache[i]=sd->shader_cache[--sd->shader_cache_count];
            return; }
}

/* ── vkCreateShaderModule: cache patchable stages, pass through unmodified ── */
VKAPI_ATTR VkResult VKAPI_CALL
stereo_CreateShaderModule(
    VkDevice device, const VkShaderModuleCreateInfo *pCI,
    const VkAllocationCallbacks *pAlloc, VkShaderModule *pSM)
{
    StereoDevice *sd=stereo_device_from_handle(device);
    if (!sd) return VK_ERROR_DEVICE_LOST;

    VkResult res=sd->real.CreateShaderModule(sd->real_device,pCI,pAlloc,pSM);
    if (res!=VK_SUCCESS) return res;

    if (!sd->stereo.enabled) return VK_SUCCESS;

    const uint32_t *spv=(const uint32_t*)pCI->pCode;
    size_t          wc =pCI->codeSize/4;
    if (is_patchable_spv(spv,wc))
        cache_add(sd,*pSM,spv,wc);
    return VK_SUCCESS;
}

/* ── vkCreateGraphicsPipelines: patch last pre-rast stage ───────────────── */
VKAPI_ATTR VkResult VKAPI_CALL
stereo_CreateGraphicsPipelines(
    VkDevice device, VkPipelineCache pc,
    uint32_t N, const VkGraphicsPipelineCreateInfo *pCI,
    const VkAllocationCallbacks *pAlloc, VkPipeline *pP)
{
    StereoDevice *sd=stereo_device_from_handle(device);
    if (!sd) return VK_ERROR_DEVICE_LOST;
    STEREO_LOG("CreateGraphicsPipelines: N=%u stereo=%d",N,sd->stereo.enabled);

    if (!sd->stereo.enabled)
        return sd->real.CreateGraphicsPipelines(sd->real_device,pc,N,pCI,pAlloc,pP);

    VkShaderModule               *tmods  =calloc(N,sizeof(VkShaderModule));
    VkPipelineShaderStageCreateInfo **tst=calloc(N,sizeof(void*));
    VkGraphicsPipelineCreateInfo    *infos=malloc(N*sizeof(*infos));
    if (!tmods||!tst||!infos){ free(tmods);free(tst);free(infos); return VK_ERROR_OUT_OF_HOST_MEMORY; }
    memcpy(infos,pCI,N*sizeof(*infos));

    const char *dump=stereo_getenv("VKS3D_DUMP_SPIRV");
    static int dump_n=0;

    for (uint32_t p=0;p<N;p++) {
        const VkGraphicsPipelineCreateInfo *ci=&pCI[p];

        /* Find last pre-rast stage: GS(2)>TES(1)>VS(0) */
        int best=-1; uint32_t bst=~0u;
        for (uint32_t s=0;s<ci->stageCount;s++) {
            VkShaderStageFlagBits sf=ci->pStages[s].stage;
            int pr=(sf==VK_SHADER_STAGE_GEOMETRY_BIT)               ?2:
                   (sf==VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT)?1:
                   (sf==VK_SHADER_STAGE_VERTEX_BIT)                 ?0:-1;
            if(pr>best){best=pr;bst=s;}
        }
        if (bst==~0u) continue;
        STEREO_LOG("Pipeline %u: last pre-rast stage=%u (GS/TES/VS prio=%d) mod=%p",
                   p,bst,best,(void*)(uintptr_t)ci->pStages[bst].module);

        StereoShaderCache *e=cache_find(sd,ci->pStages[bst].module);
        if (!e){ STEREO_LOG("Pipeline %u: stage %u not cached",p,bst); continue; }

        uint32_t *patched=NULL; size_t pc2=0;
        if (!spirv_patch_stereo_vertex(e->spv,e->words,&patched,&pc2,
                sd->stereo.left_eye_offset,sd->stereo.right_eye_offset,
                sd->stereo.convergence,sd->stereo.multiview))
            { STEREO_LOG("Pipeline %u: patch failed",p); continue; }

        if (dump) {
            char path[512];
            _snprintf(path,sizeof(path)-1,"%s\\pipe%04d_s%u.spv",dump,dump_n++,bst);
            FILE *f=fopen(path,"wb");
            if(f){fwrite(patched,4,pc2,f);fclose(f);STEREO_LOG("Dumped %s",path);}
        }

        VkShaderModuleCreateInfo smci={
            .sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
            .codeSize=pc2*4,.pCode=patched};
        VkShaderModule tmp=VK_NULL_HANDLE;
        VkResult mr=sd->real.CreateShaderModule(sd->real_device,&smci,NULL,&tmp);
        spirv_patched_free(patched);
        if (mr!=VK_SUCCESS){STEREO_ERR("Pipeline %u: tmp module failed %d",p,mr); continue;}

        uint32_t sc=ci->stageCount;
        VkPipelineShaderStageCreateInfo *st=malloc(sc*sizeof(*st));
        if(!st){sd->real.DestroyShaderModule(sd->real_device,tmp,NULL); continue;}
        memcpy(st,ci->pStages,sc*sizeof(*st));
        st[bst].module=tmp;
        infos[p].pStages=st; tmods[p]=tmp; tst[p]=st;
        STEREO_LOG("Pipeline %u: patched stage %u → tmp=%p",p,bst,(void*)(uintptr_t)tmp);
    }

    VkResult res=sd->real.CreateGraphicsPipelines(sd->real_device,pc,N,infos,pAlloc,pP);
    STEREO_LOG("CreateGraphicsPipelines result=%d",res);

    /* Pool the tmp modules — do NOT destroy them here.
     * Driver 426.06 retains a reference to the shader module's SPIR-V even
     * after CreateGraphicsPipelines returns (technically non-conformant but
     * observed on old NVIDIA drivers).  Destroying the module immediately
     * causes the driver to silently fall back to unpatched code, producing
     * mono output.  Modules are released in bulk at stereo_DestroyDevice. */
    for (uint32_t p=0;p<N;p++){
        if(tmods[p]){
            if(sd->tmp_module_count < MAX_TMP_MODULES)
                sd->tmp_modules[sd->tmp_module_count++]=tmods[p];
            else{
                /* Pool full — destroy immediately as last resort */
                STEREO_ERR("tmp_module pool full — destroying module %p immediately",
                           (void*)(uintptr_t)tmods[p]);
                sd->real.DestroyShaderModule(sd->real_device,tmods[p],NULL);
            }
        }
        free(tst[p]);
    }
    free(tmods);free(tst);free(infos);
    return res;
}

/* ── vkDestroyShaderModule ───────────────────────────────────────────────── */
VKAPI_ATTR void VKAPI_CALL
stereo_DestroyShaderModule(VkDevice device, VkShaderModule sm,
                           const VkAllocationCallbacks *pAlloc)
{
    StereoDevice *sd=stereo_device_from_handle(device);
    if(!sd) return;
    cache_remove(sd,sm);
    sd->real.DestroyShaderModule(sd->real_device,sm,pAlloc);
}
