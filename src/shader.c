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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include "stereo_icd.h"
#include "tes_inject.h"

/* ── SPIR-V opcodes / constants ──────────────────────────────────────────── */
#define SpvOpCapability         17
#define SpvOpEntryPoint         15
#define SpvOpTypeBool           20
#define SpvOpTypeInt            21
#define SpvOpTypeFloat          22
#define SpvOpTypeVector         23
#define SpvOpTypeMatrix         24
#define SpvOpTypePointer        32
#define SpvOpTypeArray          28
#define SpvOpConstant           43
#define SpvOpVariable           59
#define SpvOpLoad               61
#define SpvOpStore              62
#define SpvOpAccessChain        65
#define SpvOpDecorate           71
#define SpvOpMemberDecorate     72
#define SpvOpFunction           54
#define SpvOpEmitVertex         218
#define SpvOpCompositeExtract   81
#define SpvOpCompositeInsert    82
#define SpvOpFAdd               129
#define SpvOpFMul               133
#define SpvOpMatrixTimesVector  145
#define SpvOpMatrixTimesMatrix  146
#define SpvOpDot                148
#define SpvOpIEqual             170
#define SpvOpINotEqual          171
#define SpvOpSelect             169
#define SpvOpReturn             253

#define SpvDecorationBuiltIn    11
#define SpvBuiltInPosition      0
#define SpvBuiltInViewIndex     4440
#define SpvStorageInput         1
#define SpvStorageOutput        3
#define SpvExecVertex           0
#define SpvExecTessEval         2
#define SpvExecGeometry         3
#define SpvCapabilityMultiView  4439
#define SPIRV_MAGIC             0x07230203u

/* ── Dynamic word buffer ─────────────────────────────────────────────────── */
typedef struct { uint32_t *w; size_t n, cap; } SpvBuf;
static bool sb_init(SpvBuf *b, size_t c)
    { b->w=malloc(c*4); b->n=0; b->cap=c; return !!b->w; }
static void sb_free(SpvBuf *b)
    { free(b->w); b->w=NULL; b->n=b->cap=0; }
static bool sb_push(SpvBuf *b, uint32_t v) {
    if (b->n>=b->cap){uint32_t*p=realloc(b->w,b->cap*8);if(!p)return false;b->w=p;b->cap*=2;}
    b->w[b->n++]=v; return true; }
static bool sb_push_n(SpvBuf *b, const uint32_t *v, size_t c)
    { for(size_t i=0;i<c;i++) if(!sb_push(b,v[i])) return false; return true; }
static inline uint32_t op_(uint32_t op, uint32_t wc) { return (wc<<16)|op; }

/* ── Module scanner ──────────────────────────────────────────────────────── */
typedef struct {
    const uint32_t *words; size_t count;
    uint32_t bound;
    bool is_patchable, has_mv_cap;

    /* Diagnostics */
    bool has_emit_vertex;
    bool has_viewindex_builtin;

    /* Geometry classification */
    bool has_matrix_ops;
    bool has_direct_position_write;
    uint32_t dot_count;

    int  exec_model;
    uint32_t pos_var, pos_member_idx, pos_ptr_type;

    /* TES shaders may contain both an input gl_in[] Position block
       and an output gl_PerVertex Position block. */
    uint32_t pos_block_type[8];
    uint32_t pos_block_count;
    bool     pos_is_block;
    uint32_t view_var, ft, v4t, it, bt, ptr_out_v4, ptr_in_int;
    size_t   fn_word;
    uint32_t emit_count;
} SpvMod;

static void do_scan(SpvMod *m, bool p2)
{
    const uint32_t *w=m->words;
    for (size_t i=5;i<m->count;) {
        uint32_t op=w[i]&0xffff, wc=w[i]>>16;
        if (!wc||i+wc>m->count) break;
        if (!p2) switch(op) {
        case SpvOpDot:
            m->dot_count++;
            break;
        case SpvOpCapability:
            if(wc>=2&&w[i+1]==SpvCapabilityMultiView) m->has_mv_cap=true; break;
        case SpvOpEntryPoint:
            if(wc>=2){uint32_t e=w[i+1];
                if(e==SpvExecVertex||e==SpvExecTessEval||e==SpvExecGeometry)
                    {m->is_patchable=true;m->exec_model=(int)e;}} break;
        case SpvOpTypeFloat:
            if(wc==3&&w[i+2]==32) m->ft=w[i+1]; break;
        case SpvOpTypeVector:
            if(wc==4&&w[i+2]==m->ft&&w[i+3]==4) m->v4t=w[i+1]; break;
        case SpvOpTypeInt:
            if(wc==4&&w[i+2]==32) m->it=w[i+1]; break;
        case SpvOpTypeMatrix:
            m->has_matrix_ops = true;
            break;

        case SpvOpMatrixTimesVector:
        case SpvOpMatrixTimesMatrix:
            STEREO_LOG(
                "MATRIX_OPCODE op=%u word=%u exec=%u pos=%u pos_block=%u",
                op,
                i,
                m->exec_model,
                m->pos_var,
                m->pos_is_block);
            m->has_matrix_ops = true;
            break;
        case SpvOpTypePointer:
            if(wc>=4){
                if(w[i+2]==SpvStorageOutput&&m->v4t&&w[i+3]==m->v4t) m->ptr_out_v4=w[i+1];
                if(w[i+2]==SpvStorageInput &&m->it  &&w[i+3]==m->it ) m->ptr_in_int=w[i+1];
            } break;
        case SpvOpDecorate:
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
        case SpvOpFunction: if(!m->fn_word) m->fn_word=i; break;
        case SpvOpEmitVertex:
            m->emit_count++;
            m->has_emit_vertex = true;
            break;
        case SpvOpStore:
        {
            if (wc >= 3 &&
                w[i+1] == m->pos_var)
            {
                uint32_t source = w[i+2];

                if (!m->has_matrix_ops)
                    m->has_direct_position_write = true;
            }
        }
        break;
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
    m->bound = m->words[3];

    /* First pass: discover decorations/types. */
    do_scan(m,false);

    /* Run again now that block Position info is known.
       Some TES shaders declare OpTypePointer before
       the OpMemberDecorate(BuiltIn Position). */
    do_scan(m,false);

    if (m->pos_is_block)
        do_scan(m,true);
}

uint64_t hash_spv(const uint32_t *data, size_t words)
{
    uint64_t h = 1469598103934665603ULL; // FNV offset basis
    for (size_t i = 0; i < words; i++) {
        uint64_t v = data[i];
        h ^= v;
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

    sd->cb_track[sd->cb_track_count].cb = cb;
    sd->cb_track[sd->cb_track_count].pipeline = pipe;
    sd->cb_track[sd->cb_track_count].render_pass = VK_NULL_HANDLE;
    sd->cb_track[sd->cb_track_count].subpass = 0;
    sd->cb_track[sd->cb_track_count].render_pass = VK_NULL_HANDLE;
    sd->cb_track[sd->cb_track_count].framebuffer = VK_NULL_HANDLE;
    sd->cb_track_count++;
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

    sd->cb_track[sd->cb_track_count].cb = cb;
    sd->cb_track[sd->cb_track_count].pipeline = VK_NULL_HANDLE;
    sd->cb_track[sd->cb_track_count].render_pass = rp;
    sd->cb_track[sd->cb_track_count].subpass = subpass;
    sd->cb_track_count++;
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
    if (sd->pipeline_info_count == sd->pipeline_info_capacity)
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

    StereoPipelineInfo *info =
        &sd->pipeline_info[sd->pipeline_info_count++];

    memset(info, 0, sizeof(*info));

    return info;
}

/* ── Stereo offset injection body ────────────────────────────────────────── */
typedef struct {
    SpvMod  *m;
    bool     have_view;

    uint32_t uv4, uint_, bt;
    uint32_t cz;
    uint32_t cf0;
    uint32_t cl;
    uint32_t cr;
    uint32_t cc;

    uint32_t projection_mode;
    /* diagnostics only */
    float lo_dbg;
    float ro_dbg;
    int   flip_dbg;
} BodyCtx;

typedef struct StereoDebugCtx {
    uint32_t pipeline_index;
    VkRenderPass render_pass;
    int is_multiview;
    uint32_t stage;
} StereoDebugCtx;

static void emit_body(SpvBuf *out, const BodyCtx *c, uint32_t *nid)
{
    STEREO_LOG(
        "EMIT_STEREO stage=%u pos=%u view=%u block=%u",
        (unsigned)c->m->exec_model,
        c->m->pos_var,
        c->m->view_var,
        c->m->pos_is_block);
    SpvMod *m=c->m;
    STEREO_LOG(
        "[EMIT] flip=%d lo=%f ro=%f proj=%d",
        c->flip_dbg,
        c->lo_dbg,
        c->ro_dbg,
        c->projection_mode);
    uint32_t ch=(*nid)++, lp=(*nid)++;
    uint32_t pptr;
    if (m->pos_is_block) {
        uint32_t mid = (m->pos_member_idx==0) ? c->cz : (*nid)++;
        if (m->pos_member_idx!=0) {
            uint32_t ci[]={op_(SpvOpConstant,4),m->it,mid,m->pos_member_idx};
            sb_push_n(out,ci,4);
        }
        uint32_t a[]={op_(SpvOpAccessChain,5),c->uv4,ch,m->pos_var,mid};
        sb_push_n(out,a,5); pptr=ch;
    } else { pptr=m->pos_var; }
    { uint32_t w[]={op_(SpvOpLoad,4),m->v4t,lp,pptr}; sb_push_n(out,w,4); }

    /* Depth-varying stereo: add a constant to clip-space x.
     * After perspective divide by w, this becomes offset/w — near vertices
     * get more parallax than far vertices, giving true 3D depth perception.
     * (The old offset*w formula produced constant NDC shift at all depths,
     * making every surface appear at the same flat stereo depth plane.)    */
    uint32_t lv=c->have_view?(*nid)++:0, isl=c->have_view?(*nid)++:0,
             sel=(*nid)++,
             px=(*nid)++, nx=(*nid)++, np=(*nid)++;
    if (c->have_view && m->view_var && m->it && c->bt) {
        if (hash_spv(m->words, m->count) == 0xc3c35ab856282a97ULL)
        {
            STEREO_LOG(
                "DXVK_UI_EMIT have_view=%d view_var=%u projection=%d",
                c->have_view,
                m->view_var,
                c->projection_mode);
        }
        { uint32_t w[]={op_(SpvOpLoad,4),m->it,lv,m->view_var};         sb_push_n(out,w,4); }
        { uint32_t w[]={op_(SpvOpIEqual,5),c->bt,isl,lv,c->cz};        sb_push_n(out,w,5); }
        STEREO_LOG(
            "emit_body: projection=%d have_view=%d view_var=%u",
            c->projection_mode,
            c->have_view,
            m->view_var);
        { uint32_t w[]={op_(SpvOpSelect,6),m->ft,sel,isl,c->cr,c->cl}; sb_push_n(out,w,6); }
    } else { sel=c->cl; }
    { uint32_t w[]={op_(SpvOpCompositeExtract,5),m->ft,px,lp,0u};    sb_push_n(out,w,5); }
    if (c->projection_mode == STEREO_PROJECTION_PARALLEL)
    {
        uint32_t w[]={op_(SpvOpFAdd,5),m->ft,nx,px,sel};
        sb_push_n(out,w,5);
    }
    else
    {
        uint32_t pw      = (*nid)++;
        uint32_t convmag = (*nid)++;
        uint32_t negconv = (*nid)++;
        uint32_t convsel = (*nid)++;
        uint32_t tmp     = (*nid)++;

        /* pw = clip.w */
        {
            uint32_t w[]={op_(SpvOpCompositeExtract,5),
                          m->ft,pw,lp,3u};
            sb_push_n(out,w,5);
        }

        /* convmag = clip.w * convergence */
        {
            uint32_t w[]={op_(SpvOpFMul,5),
                          m->ft,convmag,pw,c->cc};
            sb_push_n(out,w,5);
        }

        /* negconv = 0.0 - convmag */
        {
            uint32_t w[]={op_(131,5),
                          m->ft,negconv,c->cf0,convmag};
            sb_push_n(out,w,5);
        }

        /* left eye = +convmag, right eye = -convmag */
        {
            uint32_t w[]={op_(SpvOpSelect,6),
                          m->ft,convsel,
                          isl,
                          convmag,
                          negconv};
            sb_push_n(out,w,6);
        }

        /* tmp = px + eyeOffset */
        {
            uint32_t w[]={op_(SpvOpFAdd,5),
                          m->ft,tmp,px,sel};
            sb_push_n(out,w,5);
        }

        /* nx = tmp - signed convergence */
        {
            uint32_t w[]={op_(131,5),   /* OpFSub */
                          m->ft,nx,tmp,convsel};
            sb_push_n(out,w,5);
        }
    }
    { uint32_t w[]={op_(SpvOpCompositeInsert,6),m->v4t,np,nx,lp,0u}; sb_push_n(out,w,6); }
    { uint32_t w[]={op_(SpvOpStore,3),pptr,np};                      sb_push_n(out,w,3); }
    STEREO_LOG(
        "emit_body complete projection=%d view=%d",
        c->projection_mode,
        c->have_view);
}

/* ── Public patcher ──────────────────────────────────────────────────────── */
bool spirv_patch_stereo_vertex(
    const uint32_t *in, size_t in_c,
    uint32_t **out, size_t *out_c,
    float lo, float ro,
    float conv,
    bool inj_vi,
    const StereoDebugCtx *dbg)
{
    const int projection_mode = STEREO_PROJECTION_OFF_AXIS;

    STEREO_LOG(
        "Projection=%s lo=%f ro=%f conv=%f",
        projection_mode == STEREO_PROJECTION_OFF_AXIS ?
            "off-axis" : "parallel",
        lo,
        ro,
        conv);
    if (!in||in_c<5||in[0]!=SPIRV_MAGIC) return false;

    SpvMod m={0};
    m.words=in;
    m.count=in_c;
    spv_scan(&m);
    STEREO_LOG(
        "SCAN exec=%u patchable=%d pos=%u posBlock=%d posMember=%u view=%u emit=%u matrix=%d directPos=%d dots=%u",
        m.exec_model,
        m.is_patchable,
        m.pos_var,
        m.pos_is_block,
        m.pos_member_idx,
        m.view_var,
        m.emit_count,
        m.has_matrix_ops,
        m.has_direct_position_write,
        m.dot_count);
    uint64_t spv_hash = hash_spv(m.words, m.count);
    {
        static int skip_list_init = 0;
        static char skip_list[1024];

        if (!skip_list_init)
        {
            const char *env = stereo_getenv("VKS3D_SKIP_SHADER_PATCHES");
            if (env)
            {
                strncpy(skip_list, env, sizeof(skip_list) - 1);
                skip_list[sizeof(skip_list) - 1] = '\0';
            }

            STEREO_LOG(
                "SKIP_SHADER_LIST=\"%s\"",
                skip_list);

            skip_list_init = 1;
        }

        if (skip_list[0])
        {
            char hashstr[17];
            snprintf(hashstr, sizeof(hashstr), "%016llx",
                (unsigned long long)spv_hash);

            if (strstr(skip_list, hashstr))
            {
                STEREO_LOG(
                    "SKIP_SHADER_PATCH hash=%s",
                    hashstr);
                return false;
            }
        }
    }
    if (spv_hash == 0xc3c35ab856282a97ULL)
    {
        STEREO_LOG(
            "DXVK_UI_CANDIDATE hash=%016llx matrix=%d pos_block=%d pos_member=%u view=%u exec=%u",
            (unsigned long long)spv_hash,
            m.has_matrix_ops,
            m.pos_is_block,
            m.pos_member_idx,
            m.view_var,
            (unsigned)m.exec_model);
    }
    /* TEMP: shader blacklist for debugging.
     * Return the original shader unchanged so we can identify which
     * patched shader is responsible for the remaining stereo artifact.
     */

    //Flatten ShadowMap.exe world geometry
    // if (spv_hash == 0xe019379afc782113ull)
    // {
    //     STEREO_LOG(
    //         "BLACKLIST shader=%016llx",
    //         (unsigned long long)spv_hash);
    //     return false;
    // }

    ////Flatten ShadowMap.exe UI
    //if (spv_hash == 0x1194cbb18ed7990full)
    //{
    //    STEREO_LOG(
    //        "BLACKLIST shader=%016llx",
    //        (unsigned long long)spv_hash);
    //    return false;
    //}
    ////Flatten SimpleSample.exe UI
    //if (spv_hash == 0xc3c35ab856282a97ull)
    //{
    //    STEREO_LOG(
    //        "BLACKLIST shader=%016llx",
    //        (unsigned long long)spv_hash);
    //    return false;
    //}

    ////Flatten RBR UI
    //if (spv_hash == 0x898ca1de82f2ced7ull)
    //{
    //    STEREO_LOG(
    //        "BLACKLIST shader=%016llx",
    //        (unsigned long long)spv_hash);
    //    return false;
    //}

    if (dbg)
    {
        STEREO_LOG(
            "PATCH_CTX hash=%016llx pipe=%u stage=%u rp=%p mv=%d",
            (unsigned long long)spv_hash,
            dbg->pipeline_index,
            dbg->stage,
            (void*)dbg->render_pass,
            dbg->is_multiview);
    }
    STEREO_LOG(
    "PATCHABLE hash=%016llx words=%zu exec=%u matrix=%d direct=%d dots=%u block=%d emits=%u pos=%u view=%u",
        (unsigned long long)spv_hash,
        m.count,
        m.exec_model,
        m.has_matrix_ops,
        m.has_direct_position_write,
        m.dot_count,
        m.pos_is_block,
        m.emit_count,
        m.pos_var,
        m.view_var);
    if (dbg) {
        STEREO_LOG(
            "PATCH_CTX pipe=%u stage=%u renderPass=%p multiview=%d",
            dbg->pipeline_index,
            dbg->stage,
            (void*)dbg->render_pass,
            dbg->is_multiview);
    
        if (!dbg->is_multiview) {
            STEREO_LOG(
                "PATCH_SKIP non-multiview render pass");
            return false;
        }
    }

    if (m.exec_model == SpvExecVertex)
    {
        STEREO_LOG(
            "SPIRV classify: vertex shader matrix_ops=%d",
            m.has_matrix_ops);
    }

    /* HUD/text/fullscreen shaders often write clip-space coordinates
     * directly and contain no matrix math. Stereoizing them pushes
     * them in front of the screen and causes excessive negative
     * parallax.
     *
     * Examples:
     *   gl_Position = vec4(pos.xy, 0.0, 1.0);
     *
     * Leave these monoscopic at screen depth.
     */
    if (m.exec_model == SpvExecVertex)
    {
        if (!m.pos_var)
        {
            STEREO_LOG("Skipping stereo patch: no gl_Position detected");
            return false;
        }
    
        /* Only reject truly screen-aligned fullscreen quads */
        if (m.pos_is_block && !m.has_matrix_ops)
        {
            STEREO_LOG("Skipping stereo patch: confirmed screen-space (pos block)");
            return false;
        }
    
        /* IMPORTANT: DO NOT rely on matrix_ops for DXVK */
    }

    if (!m.is_patchable || !m.pos_var)
        return false;

    if (!m.is_patchable || !m.pos_var)
        return false;

    /* Avoid stereoizing helper/fullscreen shaders that directly
     * write clip-space positions. These are responsible for the
     * duplicated shadow/composite artifacts seen in deferredshadows.
     */
/*     if (!m.has_matrix_ops && m.exec_model != SpvExecutionModelVertex) {*/
/*         STEREO_LOG("Skipping stereo patch: no matrix operations detected");*/
/*         return false;*/
/*     }*/

    bool is_gs = (m.exec_model == SpvExecGeometry);

    uint32_t nid=m.bound;
    uint32_t id_ptr_v4=nid++, id_ptr_int=nid++;
    uint32_t id_new_it=0;
    if (!m.it && inj_vi && !m.view_var) { id_new_it=nid++; m.it=id_new_it; }

    bool     will_inj_vi = inj_vi && !m.view_var && m.it;
    uint32_t id_inj_view = will_inj_vi ? nid++ : 0;
    bool     have_view   = (m.view_var || will_inj_vi);
    uint32_t id_new_bt=0;
    if (!m.bt && have_view && m.it) id_new_bt=nid++;

    uint32_t id_cz=nid++,
         id_cf0=nid++,
         id_cl=nid++,
         id_cr=nid++,
         id_cc=nid++;
    uint32_t uv4  = m.ptr_out_v4 ? m.ptr_out_v4 : id_ptr_v4;
    uint32_t uint_= m.ptr_in_int  ? m.ptr_in_int  : id_ptr_int;
    uint32_t bt   = m.bt          ? m.bt          : id_new_bt;

    SpvBuf te; if (!sb_init(&te,96)) return false;
    if (id_new_it) { uint32_t w[]={op_(SpvOpTypeInt,4),id_new_it,32,1}; sb_push_n(&te,w,4); }
    if (!m.ptr_out_v4) {
        uint32_t w[]={op_(SpvOpTypePointer,4),id_ptr_v4,SpvStorageOutput,m.v4t};
        sb_push_n(&te,w,4); }
    if (m.it && !m.ptr_in_int) {
        uint32_t w[]={op_(SpvOpTypePointer,4),id_ptr_int,SpvStorageInput,m.it};
        sb_push_n(&te,w,4); m.ptr_in_int=id_ptr_int; uint_=id_ptr_int; }
    if (id_new_bt) { uint32_t w[]={op_(SpvOpTypeBool,2),id_new_bt}; sb_push_n(&te,w,2); }
    if (m.it) { uint32_t w[]={op_(SpvOpConstant,4),m.it,id_cz,0}; sb_push_n(&te,w,4); }
    STEREO_LOG(
        "[SPIRV] lo=%f ro=%f conv=%f projection=%d",
        lo,
        ro,
        conv,
        projection_mode);
    {
        uint32_t w[4]={op_(SpvOpConstant,4),m.ft,id_cf0,0};
        float z=0.0f;
        memcpy(&w[3],&z,4);
        sb_push_n(&te,w,4);
    }
    { uint32_t w[4]={op_(SpvOpConstant,4),m.ft,id_cl,0}; memcpy(&w[3],&lo,4); sb_push_n(&te,w,4); }
    { uint32_t w[4]={op_(SpvOpConstant,4),m.ft,id_cr,0}; memcpy(&w[3],&ro,4); sb_push_n(&te,w,4); }
    { uint32_t w[4]={op_(SpvOpConstant,4),m.ft,id_cc,0};
      memcpy(&w[3],&conv,4);
      sb_push_n(&te,w,4); }
    if (will_inj_vi) {
        { uint32_t d[]={op_(SpvOpDecorate,4),id_inj_view,SpvDecorationBuiltIn,SpvBuiltInViewIndex};
          sb_push_n(&te,d,4); }
        { uint32_t v[]={op_(SpvOpVariable,4),uint_,id_inj_view,SpvStorageInput};
          sb_push_n(&te,v,4); }
        m.view_var=id_inj_view;
    }

    BodyCtx bc={&m, have_view, uv4, uint_, bt,
             id_cz, id_cf0,
             id_cl, id_cr, id_cc,
             projection_mode,
             lo,
             ro,
             0};
    STEREO_LOG(
        "PATCH_BODY hash=%016llx lo=%f ro=%f conv=%f have_view=%d pos=%u",
        (unsigned long long)spv_hash,
        lo,
        ro,
        conv,
        have_view,
        m.pos_var);
    STEREO_LOG(
        "[SPIRV] build BodyCtx lo=%f ro=%f conv=%f proj=%d",
        lo,
        ro,
        conv,
        projection_mode);
    size_t ins_t=0, ins_b=0;
    for (size_t i=5;i<in_c;) {
        uint32_t opx=in[i]&0xffff, wcx=in[i]>>16;
        if (!wcx||i+wcx>in_c) break;

        if (opx==SpvOpFunction && !ins_t)
            ins_t=i;

        /* Always inject immediately before the final OpReturn.
         * Some shaders continue modifying gl_Position after its
         * last apparent OpStore via helper logic or additional
         * stores. Making the stereo adjustment the final operation
         * guarantees it survives.
         */
        if (opx==SpvOpReturn)
            ins_b=i;
        i+=wcx;
    }
    if (!ins_t) { sb_free(&te); return false; }
    if (!is_gs && !ins_b) { sb_free(&te); return false; }
    if (!is_gs && (!ins_b || ins_b < ins_t)) { sb_free(&te); return false; }

    bool need_mv_cap = id_inj_view && !m.has_mv_cap;
    bool mv_done=false, te_done=false, body_done=false;

    SpvBuf ob;
    if (!sb_init(&ob, in_c + te.n + 64)) { sb_free(&te); return false; }
    sb_push_n(&ob, in, 5);

    for (size_t i=5;i<in_c;) {
        if (!mv_done && need_mv_cap) {
            uint32_t c[]={op_(SpvOpCapability,2),SpvCapabilityMultiView};
            sb_push_n(&ob,c,2); mv_done=true; }
        if (!te_done && i==ins_t) { sb_push_n(&ob,te.w,te.n); te_done=true; }

        uint32_t opx=in[i]&0xffff, wcx=in[i]>>16;
        if (!wcx||i+wcx>in_c) break;

        if (id_inj_view && opx==SpvOpEntryPoint && wcx>=4 &&
            (in[i+1]==SpvExecVertex||in[i+1]==SpvExecGeometry||
             in[i+1]==SpvExecTessEval)) {
            bool is_vertex = (in[i+1] == SpvExecVertex);
            
            if (id_inj_view && is_vertex)
            {
                sb_push(&ob, ((wcx+1)<<16)|SpvOpEntryPoint);
                sb_push_n(&ob, &in[i+1], wcx-1);
                sb_push(&ob, id_inj_view);
            }
            else
            {
                sb_push_n(&ob, &in[i], wcx);
            }
            i+=wcx; continue;
        }

        if (is_gs && opx==SpvOpEmitVertex) emit_body(&ob, &bc, &nid);
        if (!is_gs && !body_done && i==ins_b) { emit_body(&ob, &bc, &nid); body_done=true; }

        sb_push_n(&ob, &in[i], wcx);
        i+=wcx;
    }
    if (!te_done) sb_push_n(&ob,te.w,te.n);
    sb_free(&te);
    ob.w[3]=nid;
    *out=ob.w; *out_c=ob.n;
    STEREO_LOG("Patched: model=%d  %zu->%zu words  bound=%u  vi=%d",
               m.exec_model, in_c, ob.n, nid, (int)(id_inj_view!=0));
    return true;
}

void spirv_patched_free(uint32_t *w) { free(w); }

/* ══════════════════════════════════════════════════════════════════════════
 * FS SPIR-V patcher — sampler2D → sampler2DArray + gl_ViewIndex layer
 * ══════════════════════════════════════════════════════════════════════════
 *
 * Called for FULL-SCREEN QUAD pipelines (vertexBindingDescriptionCount == 0)
 * in multiview render passes.  All VkImage 2D attachments are upgraded to
 * arrayLayers=2 by stereo_CreateImage, so ALL sampler2D in these shaders
 * (G-buffer, shadow map, lighting output) reference 2D array images.
 * We patch ALL OpTypeImage Dim=2D Arrayed=0 → Arrayed=1 and extend the
 * sampling coordinate from vec2(u,v) to vec3(u,v,gl_ViewIndex).
 *
 * Geometry pipelines (has vertex input) use the existing VS gl_ViewIndex
 * patch instead — those shaders sample material textures (not upgraded) and
 * must NOT have their sampler types changed.
 */

#define FS_MAX_IMG   64
#define FS_MAX_SI    64
#define FS_MAX_LOADS 512

typedef struct {
    uint32_t img_ids[FS_MAX_IMG];       uint32_t n_img;
    uint32_t img_depth[FS_MAX_IMG];
    uint32_t img_arrayed[FS_MAX_IMG];

    uint32_t si_ids[FS_MAX_SI];         uint32_t n_si;

    uint32_t load_ids[FS_MAX_LOADS];
    uint32_t load_bindings[FS_MAX_LOADS];
    uint32_t n_load;
    uint32_t float_id;
    uint32_t int_id;
    uint32_t v3float_id;
    uint32_t ptr_int_in_id;
    uint32_t vi_var_id;
    bool     has_mv_cap;
    size_t   ep_word;
    size_t   fn_word;
} FsScan;

static bool fs_id_in(const uint32_t *arr, uint32_t n, uint32_t id)
{
    for (uint32_t i = 0; i < n; i++) if (arr[i] == id) return true;
    return false;
}

static void fs_prescan(FsScan *s, const uint32_t *w, size_t c)
{
    memset(s, 0, sizeof(*s));
    bool in_func = false;
    for (size_t i = 5; i < c; ) {
        uint32_t op = w[i] & 0xffff, wc = w[i] >> 16;
        if (!wc || i + wc > c) break;
        switch (op) {
        case 17:  /* OpCapability */
            if (wc >= 2 && w[i+1] == 4439) s->has_mv_cap = true;
            break;
        case 15:  /* OpEntryPoint */
            if (!s->ep_word) s->ep_word = i;
            break;
        case 22:  /* OpTypeFloat 32 */
            if (wc >= 3 && w[i+2] == 32) s->float_id = w[i+1];
            break;
        case 21:  /* OpTypeInt 32 */
            if (wc >= 3 && w[i+2] == 32) s->int_id = w[i+1];
            break;
        case 23:  /* OpTypeVector float 3 */
            if (wc >= 4 && s->float_id && w[i+2] == s->float_id && w[i+3] == 3)
                s->v3float_id = w[i+1];
            break;
        case 25:  /* OpTypeImage */
        {
            if (wc >= 9)
            {
                STEREO_LOG(
                    "FS image type: id=%u dim=%u depth=%u arrayed=%u sampled=%u format=%u",
                    w[i+1],
                    w[i+3],
                    w[i+4],
                    w[i+5],
                    w[i+7],
                    w[i+8]);

                /* Existing path */
                if (w[i+3] == 1 && w[i+5] == 0 && s->n_img < FS_MAX_IMG)
                {
                    STEREO_LOG("FS accepted image type %u", w[i+1]);
                    s->img_ids[s->n_img++] = w[i+1];
                }
                else
                {
                    STEREO_LOG(
                        "FS rejected image type %u dim=%u sampled=%u",
                        w[i+1],
                        w[i+3],
                        w[i+7]);
                }
            }
        }
        break;
        case 27:  /* OpTypeSampledImage: [1]=id [2]=image_type */
            if (wc >= 3 && fs_id_in(s->img_ids, s->n_img, w[i+2]) && s->n_si < FS_MAX_SI)
                s->si_ids[s->n_si++] = w[i+1];
            break;
        case 32:  /* OpTypePointer Input int → ptr_int_in */
            if (wc >= 4 && w[i+2] == 1 && s->int_id && w[i+3] == s->int_id)
                s->ptr_int_in_id = w[i+1];
            break;
        case 71:  /* OpDecorate */
            if (wc >= 4) {

                /* BuiltIn ViewIndex */
                if (w[i+2] == 11 && w[i+3] == 4440)
                    s->vi_var_id = w[i+1];

                /* Descriptor binding */
                if (w[i+2] == 33) {
                    STEREO_LOG(
                        "FS binding: id=%u binding=%u",
                        w[i+1],
                        w[i+3]);
                }

                /* Descriptor set */
                if (w[i+2] == 34) {
                    STEREO_LOG(
                        "FS set: id=%u set=%u",
                        w[i+1],
                        w[i+3]);
                }
            }
            break;
        case 54:  /* OpFunction */
            if (!s->fn_word) s->fn_word = i;
            in_func = true;
            break;
        default:
            if (in_func) {

                /* OpLoad of patched sampled-image type */
                if (op == 61 && wc >= 4 &&
                    fs_id_in(s->si_ids, s->n_si, w[i+1]) &&
                    s->n_load < FS_MAX_LOADS)
                {
                    uint32_t idx = s->n_load++;

                    s->load_ids[idx] = w[i+2];

                    STEREO_LOG(
                        "FS OpLoad: type=%u result=%u",
                        w[i+1],
                        w[i+2]);
                }

                /* OpSampledImage combining a patched image+sampler */
                if (op == 86 && wc >= 5 &&
                    fs_id_in(s->si_ids, s->n_si, w[i+1]) &&
                    s->n_load < FS_MAX_LOADS)
                {
                    uint32_t idx = s->n_load++;

                    s->load_ids[idx] = w[i+2];

                    STEREO_LOG(
                        "FS OpSampledImage: type=%u result=%u image=%u sampler=%u",
                        w[i+1],
                        w[i+2],
                        w[i+3],
                        w[i+4]);
                }
            }
            break;
        }
        i += wc;
    }
}

static uint32_t fs_count_patches(const FsScan *s, const uint32_t *w, size_t c)
{
    uint32_t count = 0;
    bool in_func = false;
    for (size_t i = 5; i < c; ) {
        uint32_t op = w[i] & 0xffff, wc = w[i] >> 16;
        if (!wc || i + wc > c) break;

        if (op == 54)
            in_func = true;

        if (in_func &&
            wc >= 5 &&
            (op == 87 || op == 88 || op == 89 || op == 90) &&
            fs_id_in(s->load_ids, s->n_load, w[i+3]))
            count++;

        /* OpImageFetch */
        if (in_func && op == 95 && wc >= 5)
            count++;
        i += wc;
    }
    return count;
}

bool spirv_patch_stereo_fs(
    const uint32_t *in, size_t in_c,
    uint32_t **out, size_t *out_c)
{
    if (!in || in_c < 5 || in[0] != SPIRV_MAGIC) return false;

    FsScan s;
    fs_prescan(&s, in, in_c);

    if (s.n_img == 0 || !s.float_id) return false;

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
    bool types_done = false;
    bool ep_done    = false;
    bool in_func    = false;

    /* Header */
    sb_push_n(&ob, in, 5);
    ob.w[3] = new_bound;

    for (size_t i = 5; i < in_c; ) {
        uint32_t op = in[i] & 0xffff, wc = in[i] >> 16;
        if (!wc || i + wc > in_c) break;

        /* Add MultiView capability before first non-capability instruction */
        if (!mv_added && op != 17) {
            uint32_t mv[] = { (2u<<16)|17, 4439 };
            sb_push_n(&ob, mv, 2);
            mv_added = true;
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
        if (op == 25 && wc >= 9 && fs_id_in(s.img_ids, s.n_img, in[i+1])) {

            STEREO_LOG(
                "FS discovered image type id=%u depth=%u arrayed=%u sampled=%u",
                in[i+1],
                in[i+4],
                in[i+5],
                in[i+7]);

            STEREO_LOG(
                "FS converting image type id=%u depth=%u arrayed=%u",
                in[i+1],
                in[i+4],
                in[i+5]);

            sb_push_n(&ob, &in[i], wc);
            ob.w[ob.n - wc + 5] = 1; /* Arrayed */
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

        /* Extend 2D sampling coordinate to 3D for patched loads */
        if (in_func && wc >= 5 &&
            (op == 87 || op == 88 || op == 89 || op == 90) &&
            fs_id_in(s.load_ids, s.n_load, in[i+3]))
        {
            STEREO_LOG(
                "FS extending sample: op=%u sampledImage=%u coord=%u result=%u",
                op,
                in[i+3],
                in[i+4],
                in[i+2]);
            uint32_t coord_id = in[i+4];
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
            sb_push(&ob, in[i]);          /* opcode */
            sb_push(&ob, in[i+1]);        /* result type */
            sb_push(&ob, in[i+2]);        /* result id */
            sb_push(&ob, in[i+3]);        /* sampled image (unchanged) */
            sb_push(&ob, id_c3);          /* new 3D coordinate */
            if (wc > 5) sb_push_n(&ob, &in[i+5], wc-5); /* image operands */
            i += wc; continue;
        }

        /* Extend OpImageFetch ivec2 -> ivec3(x,y,ViewIndex) */
        if (in_func && op == 95 && wc >= 5)
        {
            uint32_t coord_id = in[i+4];

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

            i += wc;
            continue;
        }

        sb_push_n(&ob, &in[i], wc);
        i += wc;
    }

    ob.w[3] = samp_nid + 1;
    *out   = ob.w;
    *out_c = ob.n;
    STEREO_LOG("FS patched: %u 2D img types→arr, %u samples extended, bound %u→%u",
               s.n_img, n_patches, in[3], ob.w[3]);
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

static StereoShaderCache *cache_find(StereoDevice *sd, VkShaderModule h)
{
    for (uint32_t i=0;i<sd->shader_cache_count;i++)
        if (sd->shader_cache[i].handle==h) return &sd->shader_cache[i];
    return NULL;
}

static void cache_add(StereoDevice *sd, VkShaderModule h,
                      const uint32_t *spv, size_t words) {
    if (sd->shader_cache_count>=MAX_SHADER_CACHE) return;
    uint32_t *cp=malloc(words*4); if (!cp) return;
    memcpy(cp,spv,words*4);
    StereoShaderCache *e=&sd->shader_cache[sd->shader_cache_count++];
    e->handle=h; e->spv=cp; e->words=words;
}
static void cache_remove(StereoDevice *sd, VkShaderModule h) {
    for (uint32_t i=0;i<sd->shader_cache_count;i++)
        if (sd->shader_cache[i].handle==h) {
            free(sd->shader_cache[i].spv);
            sd->shader_cache[i]=sd->shader_cache[--sd->shader_cache_count];
            return; }
}

/* ── vkCreateShaderModule ─────────────────────────────────────────────────── */
VKAPI_ATTR VkResult VKAPI_CALL
stereo_CreateShaderModule(VkDevice device, const VkShaderModuleCreateInfo *pCI,
                          const VkAllocationCallbacks *pAlloc, VkShaderModule *pSM)
{
    StereoDevice *sd=stereo_device_from_handle(device);
    if (!sd) return VK_ERROR_DEVICE_LOST;
    VkResult res=sd->real.CreateShaderModule(sd->real_device,pCI,pAlloc,pSM);
    if (res!=VK_SUCCESS) return res;
    if (!sd->stereo.enabled) return VK_SUCCESS;
    const uint32_t *spv=(const uint32_t*)pCI->pCode;
    size_t wc=pCI->codeSize/4;
    if (is_patchable_spv(spv,wc)) cache_add(sd,*pSM,spv,wc);
    return VK_SUCCESS;
}

/* ── vkCreateGraphicsPipelines ───────────────────────────────────────────── */
VKAPI_ATTR VkResult VKAPI_CALL
stereo_CreateGraphicsPipelines(VkDevice device, VkPipelineCache pc,
    uint32_t N, const VkGraphicsPipelineCreateInfo *pCI,
    const VkAllocationCallbacks *pAlloc, VkPipeline *pP)
{
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
            in_mv_rp = (rpi != NULL && rpi->has_multiview);
        }
        STEREO_LOG(
            "PIPE_DECISION p=%u rp=%p rpi=%p in_mv=%u stages=%u has_vs=%u has_tes=%u quad=%u",
            p,
            (void*)ci->renderPass,
            (void*)rpi,
            (unsigned)in_mv_rp,
            ci->stageCount,
            (unsigned)has_vs,
            (unsigned)has_tes,
            (!ci->pVertexInputState ||
             ci->pVertexInputState->vertexBindingDescriptionCount == 0));

        /* ── PATCH 3: Pipeline multiview FIXED (NO pipeline struct exists) ─────────────── */
        /* Multiview is render-pass driven ONLY.
         * Pipeline pNext must NOT contain VkPipelineMultiviewCreateInfo (invalid Vulkan API). */
        if (in_mv_rp) {
            STEREO_LOG("Pipe %u: MV RP detected (stageCount=%u) - no pipeline pNext needed",
                       p, ci->stageCount);
            /* optional: mark via internal flag if needed later */
            infos[p].renderPass = rpi->mv_handle;
        }

        if (!in_mv_rp) {
            STEREO_LOG(
                "Pipe %u: rp=%p not multiview (VS=%d TES=%d stages=%u)",
                p,
                (void*)(uintptr_t)ci->renderPass,
                has_vs,
                has_tes,
                ci->stageCount);

            /* TEMP: continue removed for diagnostics */
        }

        /* Substitute multiview render pass for pipeline compilation.
         * Pipelines must be compiled against the MV render pass so the driver
         * enables multiview optimisation and gl_ViewIndex receives the real
         * per-view index (0 or 1).  Render-pass compatibility rules allow these
         * pipelines to be used with both MV and non-MV framebuffers since
         * viewMask is not part of the compatibility criteria. */

        if (rpi && rpi->mv_handle && rpi->has_multiview)
        {
            /* IMPORTANT: DXVK safety gate
             * Only swap renderpass if the actual active RP is MV-capable
             */
            if (in_mv_rp)
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

        if (is_quad && ci->stageCount > 0) {
            /* Find FS stage */
            uint32_t fs_s = ~0u;
            for (uint32_t s2 = 0; s2 < ci->stageCount; s2++)
                if (ci->pStages[s2].stage == VK_SHADER_STAGE_FRAGMENT_BIT)
                    { fs_s = s2; break; }
            if (fs_s == ~0u) {
                STEREO_LOG("Pipe %u: quad but no FS stage", p);
                continue;
            }
            StereoShaderCache *e = cache_find(sd, ci->pStages[fs_s].module);
            if (!e) {
                STEREO_LOG("Pipe %u: quad FS not cached (stageCount=%u)", p, ci->stageCount);
                continue;
            }
            STEREO_LOG(
                "VS_PATCH hash=%016llx words=%zu module=%p",
                (unsigned long long)hash_spv(e->spv, e->words),
                e->words,
                (void*)(has_vs ? ci->pStages[vs_stage].module : VK_NULL_HANDLE));
            if (dump) {
                uint64_t spv_hash = hash_spv(e->spv, e->words);
                char dp[512];
                _snprintf(
                    dp,
                    sizeof(dp)-1,
                    "%s\\%016llx-fs.spv",
                    dump,
                    (unsigned long long)spv_hash);
                FILE *f = fopen(dp, "wb");
                if (f) {
                    fwrite(e->spv,4,e->words,f);
                    fclose(f);
                }
            }
            uint32_t *patched = NULL; size_t pc2 = 0;
            if (!spirv_patch_stereo_fs(e->spv, e->words, &patched, &pc2)) {
                STEREO_LOG("Pipe %u: FS patch skipped (no 2D samplers — material-only?)", p);
                continue;
            }
            if (dump) {
                uint64_t spv_hash = hash_spv(e->spv, e->words);
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
            if (dump) {
                uint64_t spv_hash = hash_spv(e->spv, e->words);
                char dp[512];
                _snprintf(
                    dp,
                    sizeof(dp)-1,
                    "%s\\%016llx-ts.spv",
                    dump,
                    (unsigned long long)spv_hash);
                FILE *f = fopen(dp, "wb");
                if (f) {
                    fwrite(e->spv,4,e->words,f);
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
                    (uint32_t)VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT
                };

                if (!spirv_patch_stereo_vertex(
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

        /* ── Path B: patch VS with gl_ViewIndex ──────────────────────────
         * Works on all modern drivers. No topology change, no extra stages.
         * Replaces the TCS+TES injection approach which crashed on newer
         * drivers due to strict PerVertex block interface validation.       */
        if (ci->stageCount > 0 && has_vs && !has_tcs && vs_stage!=~0u) {
            StereoShaderCache *e=cache_find(sd, ci->pStages[vs_stage].module);
            if (!e) { STEREO_LOG("Pipe %u PathB: VS not cached",p); continue; }
            if (dump) {
                uint64_t spv_hash = hash_spv(e->spv, e->words);
                char dp[512];
                _snprintf(
                    dp,
                    sizeof(dp)-1,
                    "%s\\%016llx-vs.spv",
                    dump,
                    (unsigned long long)spv_hash);
                FILE *f = fopen(dp, "wb");
                if (f) {
                    fwrite(e->spv, 4, e->words, f);
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
            StereoDebugCtx dbgB = {
                p,
                ci->renderPass,
                in_mv_rp,
                (uint32_t)VK_SHADER_STAGE_VERTEX_BIT
            };

            if (!spirv_patch_stereo_vertex(
                    e->spv, e->words,
                    &patched, &pc2,
                    lo, ro, conv,
                    /*inj_vi=*/true,
                    &dbgB)) {
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
    VkResult res=sd->real.CreateGraphicsPipelines(sd->real_device,pc,N,infos,pAlloc,pP);
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
    StereoDevice *sd=stereo_device_from_handle(device);
    if (!sd) return;
    cache_remove(sd,sm);
    sd->real.DestroyShaderModule(sd->real_device,sm,pAlloc);
}
