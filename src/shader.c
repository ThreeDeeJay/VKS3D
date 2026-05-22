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
#include "tes_inject.h"

/* ── SPIR-V opcodes ────────────────────────────────────────────────────────── */
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
#define SpvCapPVNA       5260
#define SpvBuiltInPVNV   5261

#define SpvCapMV 5296

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
    int       exec_model;

    uint32_t  pos_var;
    bool      pos_is_block;
    uint32_t  pos_block_type, pos_member_idx, pos_ptr_type;

    uint32_t  view_var;
    uint32_t  ft, v4t, it, bt;
    uint32_t  ptr_out_v4;
    uint32_t  ptr_in_int;
    size_t    fn_word;
    uint32_t  emit_count;
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
    bool     have_view;
    uint32_t uv4;
    uint32_t uint_;
    uint32_t bt;
    uint32_t cz;
    uint32_t cl, cr;
    bool     pvna;
    uint32_t pvnv_var;
    uint32_t c1;
} BodyCtx;

/* Emit one instance of the stereo-offset body. */
static void emit_body(SpvBuf *out, const BodyCtx *c, uint32_t *nid)
{
    SpvMod *m = c->m;

    uint32_t ch   = (*nid)++;
    uint32_t lp   = (*nid)++;

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

    { uint32_t w[]={op_(SpvOpLoad,4),m->v4t,lp,pptr}; sb_push_n(out,w,4); }

    if (c->pvna && c->pvnv_var) {
        uint32_t pw   = (*nid)++;
        uint32_t lox  = (*nid)++;
        uint32_t rox  = (*nid)++;
        uint32_t px   = (*nid)++;
        uint32_t lx   = (*nid)++;
        uint32_t rx   = (*nid)++;
        uint32_t lpos = (*nid)++;
        uint32_t rpos = (*nid)++;
        uint32_t p0   = (*nid)++;
        uint32_t p1   = (*nid)++;

        { uint32_t w[]={op_(SpvOpCompositeExtract,5),m->ft,pw,lp,3u};   sb_push_n(out,w,5); }
        { uint32_t w[]={op_(SpvOpFMul,5),m->ft,lox,c->cl,pw};           sb_push_n(out,w,5); }
        { uint32_t w[]={op_(SpvOpFMul,5),m->ft,rox,c->cr,pw};           sb_push_n(out,w,5); }
        { uint32_t w[]={op_(SpvOpCompositeExtract,5),m->ft,px,lp,0u};   sb_push_n(out,w,5); }
        { uint32_t w[]={op_(SpvOpFAdd,5),m->ft,lx,px,lox};              sb_push_n(out,w,5); }
        { uint32_t w[]={op_(SpvOpFAdd,5),m->ft,rx,px,rox};              sb_push_n(out,w,5); }
        { uint32_t w[]={op_(SpvOpCompositeInsert,6),m->v4t,lpos,lx,lp,0u}; sb_push_n(out,w,6); }
        { uint32_t w[]={op_(SpvOpCompositeInsert,6),m->v4t,rpos,rx,lp,0u}; sb_push_n(out,w,6); }
        { uint32_t w[]={op_(SpvOpAccessChain,5),c->uv4,p0,c->pvnv_var,c->cz}; sb_push_n(out,w,5); }
        { uint32_t w[]={op_(SpvOpStore,3),p0,lpos};                     sb_push_n(out,w,3); }
        { uint32_t w[]={op_(SpvOpAccessChain,5),c->uv4,p1,c->pvnv_var,c->c1}; sb_push_n(out,w,5); }
        { uint32_t w[]={op_(SpvOpStore,3),p1,rpos};                     sb_push_n(out,w,3); }

    } else {
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
            STEREO_LOG("emit_body: no view index (have_view=%d view_var=%u it=%u bt=%u)",
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
    bool use_pvna = (m.exec_model == SpvExecVertex || m.exec_model == SpvExecGeometry);

    uint32_t nid = m.bound;
    uint32_t id_ptr_v4  = nid++;
    uint32_t id_ptr_int = nid++;

    uint32_t id_new_it = 0;
    if (!m.it && inj_vi && !m.view_var) {
        id_new_it = nid++;
        m.it      = id_new_it;
        STEREO_LOG("Shader: no int type found — creating id=%u for gl_ViewIndex", id_new_it);
    }

    uint32_t id_uint_type  = 0;
    uint32_t id_const_2    = 0;
    uint32_t id_v4arr2     = 0;
    uint32_t id_ptr_v4arr2 = 0;
    uint32_t id_pvnv_var   = 0;
    uint32_t id_const_1    = 0;
    if (use_pvna) {
        id_uint_type  = nid++;
        id_const_2    = nid++;
        id_v4arr2     = nid++;
        id_ptr_v4arr2 = nid++;
        id_pvnv_var   = nid++;
        id_const_1    = nid++;
    }

    bool     will_inj_vi = !use_pvna && inj_vi && !m.view_var && m.it;
    uint32_t id_inj_view = will_inj_vi ? nid++ : 0;
    bool     have_view   = !use_pvna && (m.view_var || will_inj_vi);
    uint32_t id_new_bt = 0;
    if (!use_pvna && !m.bt && have_view && m.it) { id_new_bt=nid++; }

    uint32_t id_cz = nid++;
    uint32_t id_cl = nid++;
    uint32_t id_cr = nid++;

    uint32_t uv4   = m.ptr_out_v4  ? m.ptr_out_v4  : id_ptr_v4;
    uint32_t uint_ = m.ptr_in_int   ? m.ptr_in_int   : id_ptr_int;
    uint32_t bt    = m.bt           ? m.bt           : id_new_bt;

    SpvBuf te; if (!sb_init(&te,64)) return false;

    if (id_new_it) {
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

    if (use_pvna) {
        { uint32_t w[]={op_(SpvOpTypeInt,4),id_uint_type,32,0};            sb_push_n(&te,w,4); }
        { uint32_t w[]={op_(SpvOpConstant,4),id_uint_type,id_const_2,2};  sb_push_n(&te,w,4); }
        { uint32_t w[]={op_(SpvOpTypeArray,4),id_v4arr2,m.v4t,id_const_2}; sb_push_n(&te,w,4); }
        { uint32_t w[]={op_(SpvOpTypePointer,4),id_ptr_v4arr2,SpvStorageOutput,id_v4arr2}; sb_push_n(&te,w,4); }
        { uint32_t w[]={op_(SpvOpDecorate,4),id_pvnv_var,SpvDecorationBuiltIn,SpvBuiltInPVNV}; sb_push_n(&te,w,4); }
        { uint32_t w[]={op_(SpvOpVariable,4),id_ptr_v4arr2,id_pvnv_var,SpvStorageOutput}; sb_push_n(&te,w,4); }
        if (m.it) { uint32_t w[]={op_(SpvOpConstant,4),m.it,id_const_1,1}; sb_push_n(&te,w,4); }
    }

    uint32_t inj_view_id = 0;
    if (will_inj_vi) {
        uint32_t d[]={op_(SpvOpDecorate,4),id_inj_view,SpvDecorationBuiltIn,SpvBuiltInViewIndex};
        sb_push_n(&te,d,4);
        uint32_t v[]={op_(SpvOpVariable,4),uint_,id_inj_view,SpvStorageInput};
        sb_push_n(&te,v,4);
        m.view_var=id_inj_view; inj_view_id=id_inj_view;
    }

    BodyCtx bc = { &m, have_view, uv4, uint_, bt, id_cz, id_cl, id_cr,
                   use_pvna, id_pvnv_var, id_const_1 };

    uint32_t instances = is_gs ? (m.emit_count > 0 ? m.emit_count : 1) : 1;
    (void)instances;

    size_t ins_t = 0;
    size_t ins_b = 0;
    for (size_t i=5; i<in_c; ) {
        uint32_t opx=in[i]&0xffff, wcx=in[i]>>16;
        if (!wcx||i+wcx>in_c) break;
        if (opx==SpvOpFunction && !ins_t) ins_t=i;
        if (!m.pos_is_block && opx==SpvOpStore && wcx>=3 && in[i+1]==m.pos_var)
            ins_b=i+wcx;
        i+=wcx;
    }
    if (!ins_t) { sb_free(&te); return false; }

    if (!m.pos_is_block && ins_b==0 && !is_gs) {
        for (size_t i=5; i<in_c; ) {
            uint32_t opx=in[i]&0xffff, wcx=in[i]>>16;
            if (!wcx) break;
            if (opx==253||opx==254) { ins_b=i; break; }
            i+=wcx;
        }
    }
    if (m.pos_is_block && !is_gs) {
        for (size_t i=5; i<in_c; ) {
            uint32_t opx=in[i]&0xffff, wcx=in[i]>>16;
            if (!wcx) break;
            if (opx==253||opx==254) { ins_b=i; break; }
            i+=wcx;
        }
    }
    if (!is_gs && !ins_b) { sb_free(&te); return false; }
    if (!is_gs && ins_b < ins_t) { sb_free(&te); return false; }

    bool need_mv_cap  = inj_view_id && !m.has_mv_cap;
    bool need_pvna_cap = use_pvna;
    bool mv_done      = false;
    bool pvna_done    = false;
    bool te_done      = false;
    bool vs_body_done = false;

    SpvBuf ob;
    size_t est = in_c + te.n + instances*32 + 32;
    if (!sb_init(&ob, est)) { sb_free(&te); return false; }
    sb_push_n(&ob, in, 5);
    ob.w[3] = nid + instances*12 + (use_pvna ? 16 : 0);

    for (size_t i=5; i<in_c; ) {
        if (!mv_done && need_mv_cap) {
            uint32_t c[]={op_(SpvOpCapability,2),SpvCapMV}; sb_push_n(&ob,c,2); mv_done=true; }
        if (!pvna_done && need_pvna_cap) {
            uint32_t c[]={op_(SpvOpCapability,2),SpvCapPVNA}; sb_push_n(&ob,c,2); pvna_done=true; }

        if (!te_done && i==ins_t) {
            sb_push_n(&ob,te.w,te.n); te_done=true; }

        uint32_t opx=in[i]&0xffff, wcx=in[i]>>16;
        if (!wcx||i+wcx>in_c) break;

        if (opx==SpvOpEntryPoint && wcx>=4 && inj_view_id &&
            (in[i+1]==SpvExecVertex||in[i+1]==SpvExecGeometry||in[i+1]==SpvExecTessEval)) {
            sb_push(&ob, ((wcx+1)<<16)|SpvOpEntryPoint);
            sb_push_n(&ob, &in[i+1], wcx-1);
            sb_push(&ob, inj_view_id);
            STEREO_LOG("OpEntryPoint extended: added view_var=%u", inj_view_id);
            i+=wcx; continue;
        }
        if (opx==SpvOpEntryPoint && wcx>=4 && use_pvna && id_pvnv_var &&
            (in[i+1]==SpvExecVertex||in[i+1]==SpvExecGeometry||in[i+1]==SpvExecTessEval)) {
            sb_push(&ob, ((wcx+1)<<16)|SpvOpEntryPoint);
            sb_push_n(&ob, &in[i+1], wcx-1);
            sb_push(&ob, id_pvnv_var);
            STEREO_LOG("OpEntryPoint extended: added pvnv_var=%u", id_pvnv_var);
            i+=wcx; continue;
        }

        if (is_gs && opx==SpvOpEmitVertex) {
            emit_body(&ob, &bc, &nid);
        }

        if (!is_gs && !vs_body_done && i==ins_b) {
            emit_body(&ob, &bc, &nid);
            vs_body_done=true;
        }

        sb_push_n(&ob, &in[i], wcx);
        i+=wcx;
    }
    if (!te_done) sb_push_n(&ob,te.w,te.n);
    sb_free(&te);

    ob.w[3] = nid;

    *out=ob.w; *out_c=ob.n;
    STEREO_LOG("Patched: model=%d  %zu→%zu words  bound=%u  vi=%d  pvna=%d  gs_inj=%u",
               m.exec_model, in_c, ob.n, nid, (int)(inj_view_id!=0),
               (int)use_pvna, is_gs?m.emit_count:1);
    return true;
}

void spirv_patched_free(uint32_t *w) { free(w); }

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

    VkShaderModule               *tmods  = calloc(N, sizeof(VkShaderModule));
    VkPipelineShaderStageCreateInfo **tst = calloc(N, sizeof(void*));
    VkGraphicsPipelineCreateInfo    *infos = malloc(N * sizeof(*infos));
    VkPipelineInputAssemblyStateCreateInfo *ia_mods =
        calloc(N, sizeof(*ia_mods));
    VkPipelineTessellationStateCreateInfo  *ts_mods =
        calloc(N, sizeof(*ts_mods));

    if (!tmods||!tst||!infos||!ia_mods||!ts_mods) {
        free(tmods); free(tst); free(infos); free(ia_mods); free(ts_mods);
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    memcpy(infos, pCI, N * sizeof(*infos));

    const char *dump = stereo_getenv("VKS3D_DUMP_SPIRV");
    static int  dump_n = 0;

    for (uint32_t p = 0; p < N; p++) {
        const VkGraphicsPipelineCreateInfo *ci = &pCI[p];

        bool has_vs=false, has_gs=false, has_tcs=false, has_tes=false;
        uint32_t tes_stage = ~0u;
        for (uint32_t s = 0; s < ci->stageCount; s++) {
            VkShaderStageFlagBits sf = ci->pStages[s].stage;
            if (sf == VK_SHADER_STAGE_VERTEX_BIT)                   has_vs  = true;
            if (sf == VK_SHADER_STAGE_GEOMETRY_BIT)                 has_gs  = true;
            if (sf == VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT)     has_tcs = true;
            if (sf == VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT) {
                has_tes = true; tes_stage = s;
            }
        }

        bool is_triangle_list = (ci->pInputAssemblyState &&
            ci->pInputAssemblyState->topology == 3);

        if (has_tes && tes_stage != ~0u) {
            StereoShaderCache *e = cache_find(sd, ci->pStages[tes_stage].module);
            if (!e) { STEREO_LOG("Pipeline %u: TES not cached — passthrough",p); continue; }

            uint32_t *patched=NULL; size_t pc2=0;
            if (!spirv_patch_stereo_vertex(e->spv, e->words, &patched, &pc2,
                    sd->stereo.left_eye_offset, sd->stereo.right_eye_offset,
                    sd->stereo.convergence, true))
                { STEREO_LOG("Pipeline %u: TES patch failed",p); continue; }

            VkShaderModuleCreateInfo smci = {
                VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,NULL,0,pc2*4,patched};
            VkShaderModule tmp = VK_NULL_HANDLE;
            VkResult mr = sd->real.CreateShaderModule(sd->real_device,&smci,NULL,&tmp);
            spirv_patched_free(patched);
            if (mr != VK_SUCCESS) {
                STEREO_ERR("Pipeline %u: TES tmp module failed %d",p,mr); continue; }

            uint32_t sc = ci->stageCount;
            VkPipelineShaderStageCreateInfo *st = malloc(sc * sizeof(*st));
            if (!st) { sd->real.DestroyShaderModule(sd->real_device,tmp,NULL); continue; }
            memcpy(st, ci->pStages, sc * sizeof(*st));
            st[tes_stage].module = tmp;
            infos[p].pStages = st; tmods[p] = tmp; tst[p] = st;
            STEREO_LOG("Pipeline %u: TES stereo patched → tmp=%p",
                       p, (void*)(uintptr_t)tmp);
            continue;
        }

        if (has_vs && !has_tcs && !has_tes && is_triangle_list) {
            uint32_t *tcs_spv=NULL; size_t tcs_c=0;
            if (!build_tcs_spv(&tcs_spv, &tcs_c)) {
                STEREO_LOG("Pipeline %u: build_tcs_spv failed",p); goto fallback; }

            uint32_t *base_tes=NULL; size_t base_c=0;
            if (!build_base_tes_spv(&base_tes, &base_c)) {
                free(tcs_spv); STEREO_LOG("Pipeline %u: build_base_tes failed",p);
                goto fallback; }

            uint32_t *tes_patched=NULL; size_t tes_pc=0;
            if (!spirv_patch_stereo_vertex(base_tes, base_c, &tes_patched, &tes_pc,
                    sd->stereo.left_eye_offset, sd->stereo.right_eye_offset,
                    sd->stereo.convergence, true)) {
                free(tcs_spv); free(base_tes);
                STEREO_LOG("Pipeline %u: TES stereo patch failed",p); goto fallback; }
            free(base_tes);

            if (dump) {
                char path[512];
                _snprintf(path,sizeof(path)-1,"%s\\pipe%04d_tcs.spv",dump,dump_n);
                FILE *f=fopen(path,"wb");
                if(f){fwrite(tcs_spv,4,tcs_c,f);fclose(f);}
                _snprintf(path,sizeof(path)-1,"%s\\pipe%04d_tes.spv",dump,dump_n++);
                f=fopen(path,"wb");
                if(f){fwrite(tes_patched,4,tes_pc,f);fclose(f);}
            }

            VkShaderModule tcs_mod=VK_NULL_HANDLE, tes_mod=VK_NULL_HANDLE;
            {
                VkShaderModuleCreateInfo smci = {
                    VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,NULL,0,
                    tcs_c*4, tcs_spv};
                sd->real.CreateShaderModule(sd->real_device,&smci,NULL,&tcs_mod);
                free(tcs_spv);
            }
            {
                VkShaderModuleCreateInfo smci = {
                    VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,NULL,0,
                    tes_pc*4, tes_patched};
                sd->real.CreateShaderModule(sd->real_device,&smci,NULL,&tes_mod);
                spirv_patched_free(tes_patched);
            }

            if (!tcs_mod || !tes_mod) {
                if (tcs_mod) sd->real.DestroyShaderModule(sd->real_device,tcs_mod,NULL);
                if (tes_mod) sd->real.DestroyShaderModule(sd->real_device,tes_mod,NULL);
                STEREO_LOG("Pipeline %u: TCS/TES module creation failed",p);
                goto fallback;
            }

            if (sd->tmp_module_count+1 < MAX_TMP_MODULES) {
                sd->tmp_modules[sd->tmp_module_count++] = tcs_mod;
                sd->tmp_modules[sd->tmp_module_count++] = tes_mod;
            }

            uint32_t orig_sc = ci->stageCount;
            VkPipelineShaderStageCreateInfo *st =
                malloc((orig_sc + 2) * sizeof(*st));
            if (!st) goto fallback;
            memcpy(st, ci->pStages, orig_sc * sizeof(*st));

            VkPipelineShaderStageCreateInfo tcs_stage = {
                VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                NULL, 0,
                VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT,
                tcs_mod, "main", NULL
            };
            VkPipelineShaderStageCreateInfo tes_stage_info = {
                VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                NULL, 0,
                VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT,
                tes_mod, "main", NULL
            };
            st[orig_sc]     = tcs_stage;
            st[orig_sc + 1] = tes_stage_info;

            ia_mods[p] = *ci->pInputAssemblyState;
            ia_mods[p].topology = 10;
            ia_mods[p].primitiveRestartEnable = 0;

            ts_mods[p].sType = 20;
            ts_mods[p].pNext = NULL;
            ts_mods[p].flags = 0;
            ts_mods[p].patchControlPoints = 3;

            infos[p].stageCount          = orig_sc + 2;
            infos[p].pStages             = st;
            infos[p].pInputAssemblyState = &ia_mods[p];
            infos[p].pTessellationState  = &ts_mods[p];
            tst[p] = st;

            STEREO_LOG("Pipeline %u: injected TCS+TES (TRIANGLE_LIST→PATCH_LIST, "
                       "tcs=%p tes=%p)",
                       p, (void*)(uintptr_t)tcs_mod, (void*)(uintptr_t)tes_mod);
            continue;
        }

        fallback:
        {
            int best=-1; uint32_t bst=~0u;
            for (uint32_t s=0;s<ci->stageCount;s++) {
                VkShaderStageFlagBits sf=ci->pStages[s].stage;
                int pr=(sf==VK_SHADER_STAGE_GEOMETRY_BIT)               ?2:
                       (sf==VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT)?1:
                       (sf==VK_SHADER_STAGE_VERTEX_BIT)                 ?0:-1;
                if(pr>best){best=pr;bst=s;}
            }
            if (bst==~0u) continue;

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
                if(f){fwrite(patched,4,pc2,f);fclose(f);}
            }

            VkShaderModuleCreateInfo smci={VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                NULL,0,pc2*4,patched};
            VkShaderModule tmp=VK_NULL_HANDLE;
            VkResult mr=sd->real.CreateShaderModule(sd->real_device,&smci,NULL,&tmp);
            spirv_patched_free(patched);
            if (mr!=VK_SUCCESS) {
                STEREO_ERR("Pipeline %u: tmp module failed %d",p,mr); continue; }

            uint32_t sc=ci->stageCount;
            VkPipelineShaderStageCreateInfo *st=malloc(sc*sizeof(*st));
            if(!st){sd->real.DestroyShaderModule(sd->real_device,tmp,NULL); continue;}
            memcpy(st,ci->pStages,sc*sizeof(*st));
            st[bst].module=tmp;
            infos[p].pStages=st; tmods[p]=tmp; tst[p]=st;
            STEREO_LOG("Pipeline %u: fallback patch stage %u → tmp=%p",
                       p,bst,(void*)(uintptr_t)tmp);
        }
    }

    VkResult res = sd->real.CreateGraphicsPipelines(sd->real_device,pc,N,infos,pAlloc,pP);
    STEREO_LOG("CreateGraphicsPipelines result=%d", res);

    for (uint32_t p=0;p<N;p++) {
        if (tmods[p]) {
            if (sd->tmp_module_count < MAX_TMP_MODULES)
                sd->tmp_modules[sd->tmp_module_count++] = tmods[p];
            else {
                STEREO_ERR("tmp_module pool full");
                sd->real.DestroyShaderModule(sd->real_device,tmods[p],NULL);
            }
        }
        free(tst[p]);
    }
    free(tmods); free(tst); free(infos); free(ia_mods); free(ts_mods);
    return res;
}

VKAPI_ATTR void VKAPI_CALL
stereo_DestroyShaderModule(VkDevice device, VkShaderModule sm,
                           const VkAllocationCallbacks *pAlloc)
{
    StereoDevice *sd=stereo_device_from_handle(device);
    if(!sd) return;
    cache_remove(sd,sm);
    sd->real.DestroyShaderModule(sd->real_device,sm,pAlloc);
}
