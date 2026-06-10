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
#define SpvOpCapability       17
#define SpvOpEntryPoint       15
#define SpvOpTypeBool         20
#define SpvOpTypeInt          21
#define SpvOpTypeFloat        22
#define SpvOpTypeVector       23
#define SpvOpTypePointer      32
#define SpvOpTypeArray        28
#define SpvOpConstant         43
#define SpvOpVariable         59
#define SpvOpLoad             61
#define SpvOpStore            62
#define SpvOpAccessChain      65
#define SpvOpDecorate         71
#define SpvOpMemberDecorate   72
#define SpvOpFunction         54
#define SpvOpEmitVertex       218
#define SpvOpCompositeExtract 81
#define SpvOpCompositeInsert  82
#define SpvOpFAdd             129
#define SpvOpFMul             133
#define SpvOpIEqual           170
#define SpvOpSelect           169

#define SpvDecorationBuiltIn   11
#define SpvBuiltInPosition      0
#define SpvBuiltInViewIndex  4440
#define SpvStorageInput         1
#define SpvStorageOutput        3
#define SpvExecVertex           0
#define SpvExecTessEval         2
#define SpvExecGeometry         3
#define SpvCapMV             5296
#define SPIRV_MAGIC  0x07230203u

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
    int  exec_model;
    uint32_t pos_var, pos_block_type, pos_member_idx, pos_ptr_type;
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
        case SpvOpCapability:
            if(wc>=2&&w[i+1]==SpvCapMV) m->has_mv_cap=true; break;
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
        case SpvOpTypeBool:
            if(wc==2) m->bt=w[i+1]; break;
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
                {m->pos_block_type=w[i+1];m->pos_member_idx=w[i+2];
                 m->pos_is_block=true;m->pos_var=0;} break;
        case SpvOpFunction: if(!m->fn_word) m->fn_word=i; break;
        case SpvOpEmitVertex: m->emit_count++; break;
        } else {
            if(op==SpvOpTypePointer&&wc>=4&&w[i+2]==SpvStorageOutput
               &&m->pos_block_type&&w[i+3]==m->pos_block_type) m->pos_ptr_type=w[i+1];
            if(op==SpvOpVariable&&wc>=4&&w[i+3]==SpvStorageOutput
               &&m->pos_ptr_type&&w[i+1]==m->pos_ptr_type) m->pos_var=w[i+2];
        }
        i+=wc;
    }
}
static void spv_scan(SpvMod *m)
    { m->bound=m->words[3]; do_scan(m,false); if(m->pos_is_block) do_scan(m,true); }

/* ── Stereo offset injection body ────────────────────────────────────────── */
typedef struct {
    SpvMod  *m;
    bool     have_view;
    uint32_t uv4, uint_, bt, cz, cl, cr;
} BodyCtx;

static void emit_body(SpvBuf *out, const BodyCtx *c, uint32_t *nid)
{
    SpvMod *m=c->m;
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
        { uint32_t w[]={op_(SpvOpLoad,4),m->it,lv,m->view_var};         sb_push_n(out,w,4); }
        { uint32_t w[]={op_(SpvOpIEqual,5),c->bt,isl,lv,c->cz};        sb_push_n(out,w,5); }
        { uint32_t w[]={op_(SpvOpSelect,6),m->ft,sel,isl,c->cl,c->cr}; sb_push_n(out,w,6); }
    } else { sel=c->cl; }
    { uint32_t w[]={op_(SpvOpCompositeExtract,5),m->ft,px,lp,0u};    sb_push_n(out,w,5); }
    { uint32_t w[]={op_(SpvOpFAdd,5),m->ft,nx,px,sel};               sb_push_n(out,w,5); }
    { uint32_t w[]={op_(SpvOpCompositeInsert,6),m->v4t,np,nx,lp,0u}; sb_push_n(out,w,6); }
    { uint32_t w[]={op_(SpvOpStore,3),pptr,np};                      sb_push_n(out,w,3); }
}

/* ── Public patcher ──────────────────────────────────────────────────────── */
bool spirv_patch_stereo_vertex(
    const uint32_t *in, size_t in_c,
    uint32_t **out, size_t *out_c,
    float lo, float ro, float conv, bool inj_vi)
{
    (void)conv;
    if (!in||in_c<5||in[0]!=SPIRV_MAGIC) return false;
    SpvMod m={0}; m.words=in; m.count=in_c;
    spv_scan(&m);
    if (!m.is_patchable||!m.pos_var) return false;

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

    uint32_t id_cz=nid++, id_cl=nid++, id_cr=nid++;
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
    { uint32_t w[4]={op_(SpvOpConstant,4),m.ft,id_cl,0}; memcpy(&w[3],&lo,4); sb_push_n(&te,w,4); }
    { uint32_t w[4]={op_(SpvOpConstant,4),m.ft,id_cr,0}; memcpy(&w[3],&ro,4); sb_push_n(&te,w,4); }

    if (will_inj_vi) {
        { uint32_t d[]={op_(SpvOpDecorate,4),id_inj_view,SpvDecorationBuiltIn,SpvBuiltInViewIndex};
          sb_push_n(&te,d,4); }
        { uint32_t v[]={op_(SpvOpVariable,4),uint_,id_inj_view,SpvStorageInput};
          sb_push_n(&te,v,4); }
        m.view_var=id_inj_view;
    }

    BodyCtx bc={&m, have_view, uv4, uint_, bt, id_cz, id_cl, id_cr};

    size_t ins_t=0, ins_b=0;
    for (size_t i=5;i<in_c;) {
        uint32_t opx=in[i]&0xffff, wcx=in[i]>>16;
        if (!wcx||i+wcx>in_c) break;
        if (opx==SpvOpFunction && !ins_t) ins_t=i;
        if (!m.pos_is_block && opx==SpvOpStore && wcx>=3 && in[i+1]==m.pos_var) ins_b=i+wcx;
        i+=wcx;
    }
    if (!ins_t) { sb_free(&te); return false; }
    if (!m.pos_is_block && ins_b==0 && !is_gs) {
        for (size_t i=5;i<in_c;) {
            uint32_t opx=in[i]&0xffff, wcx=in[i]>>16; if (!wcx) break;
            if (opx==253||opx==254) { ins_b=i; break; } i+=wcx;
        }
    }
    if (m.pos_is_block && !is_gs) {
        for (size_t i=5;i<in_c;) {
            uint32_t opx=in[i]&0xffff, wcx=in[i]>>16; if (!wcx) break;
            if (opx==253||opx==254) { ins_b=i; break; } i+=wcx;
        }
    }
    if (!is_gs && !ins_b) { sb_free(&te); return false; }
    if (!is_gs && ins_b < ins_t) { sb_free(&te); return false; }

    bool need_mv_cap = id_inj_view && !m.has_mv_cap;
    bool mv_done=false, te_done=false, body_done=false;

    SpvBuf ob;
    if (!sb_init(&ob, in_c + te.n + 64)) { sb_free(&te); return false; }
    sb_push_n(&ob, in, 5);

    for (size_t i=5;i<in_c;) {
        if (!mv_done && need_mv_cap) {
            uint32_t c[]={op_(SpvOpCapability,2),SpvCapMV};
            sb_push_n(&ob,c,2); mv_done=true; }
        if (!te_done && i==ins_t) { sb_push_n(&ob,te.w,te.n); te_done=true; }

        uint32_t opx=in[i]&0xffff, wcx=in[i]>>16;
        if (!wcx||i+wcx>in_c) break;

        if (id_inj_view && opx==SpvOpEntryPoint && wcx>=4 &&
            (in[i+1]==SpvExecVertex||in[i+1]==SpvExecGeometry||
             in[i+1]==SpvExecTessEval)) {
            sb_push(&ob, ((wcx+1)<<16)|SpvOpEntryPoint);
            sb_push_n(&ob, &in[i+1], wcx-1);
            sb_push(&ob, id_inj_view);
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

/* ── Helpers ─────────────────────────────────────────────────────────────── */
static bool is_patchable_spv(const uint32_t *w, size_t c)
{
    if (c<5||w[0]!=SPIRV_MAGIC) return false;
    for (size_t i=5;i<c;) {
        uint32_t op=w[i]&0xffff, wc=w[i]>>16; if (!wc||i+wc>c) break;
        if (op==SpvOpEntryPoint&&wc>=2) {
            uint32_t e=w[i+1];
            return e==SpvExecVertex||e==SpvExecGeometry||e==SpvExecTessEval;
        }
        i+=wc;
    }
    return false;
}
static StereoShaderCache *cache_find(StereoDevice *sd, VkShaderModule h) {
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
    STEREO_LOG("CreateGraphicsPipelines: N=%u", N);
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

    const char *dump = stereo_getenv("VKS3D_DUMP_SPIRV");
    static int  dump_n = 0;
    float lo=sd->stereo.left_eye_offset, ro=sd->stereo.right_eye_offset,
          conv=sd->stereo.convergence;

    for (uint32_t p=0; p<N; p++) {
        const VkGraphicsPipelineCreateInfo *ci=&pCI[p];

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
        bool in_mv_rp = false;
        if (ci->renderPass != VK_NULL_HANDLE) {
            StereoRenderPassInfo *rpi = stereo_rp_lookup(sd, ci->renderPass);
            in_mv_rp = (rpi != NULL && rpi->has_multiview);
        }
        if (!in_mv_rp) {
            uint32_t att = ci->pColorBlendState ? ci->pColorBlendState->attachmentCount : 0;
            STEREO_LOG("Pipe %u: rp=%p NOT multiview (attachments~%u) "
                       "— center perspective (deferred/shadow/post-fx alignment)",
                       p, (void*)(uintptr_t)ci->renderPass, att);
            continue;
        }

        /* ── Path A: patch existing TES ──────────────────────────────── */
        if (has_tes && tes_stage!=~0u) {
            StereoShaderCache *e=cache_find(sd, ci->pStages[tes_stage].module);
            if (!e) { STEREO_LOG("Pipe %u PathA: TES not cached",p); continue; }
            uint32_t *patched=NULL; size_t pc2=0;
            if (!spirv_patch_stereo_vertex(e->spv,e->words,&patched,&pc2,
                    lo,ro,conv,true)) {
                STEREO_LOG("Pipe %u PathA: patch failed",p); continue; }
            if (dump) {
                char dp[512];
                _snprintf(dp,sizeof(dp)-1,"%s\\pipe%04d_a_tes.spv",dump,dump_n++);
                FILE *f=fopen(dp,"wb"); if(f){fwrite(patched,4,pc2,f);fclose(f);}
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
            st[tes_stage].module=tmp;
            infos[p].pStages=st; tmp_mod[p]=tmp; tst[p]=st;
            STEREO_LOG("Pipe %u: Path A — TES patched (gl_ViewIndex)",p);
            continue;
        }

        /* ── Path B: patch VS with gl_ViewIndex ──────────────────────────
         * Works on all modern drivers. No topology change, no extra stages.
         * Replaces the TCS+TES injection approach which crashed on newer
         * drivers due to strict PerVertex block interface validation.       */
        if (has_vs && !has_tcs && vs_stage!=~0u) {
            StereoShaderCache *e=cache_find(sd, ci->pStages[vs_stage].module);
            if (!e) { STEREO_LOG("Pipe %u PathB: VS not cached",p); continue; }
            uint32_t *patched=NULL; size_t pc2=0;
            if (!spirv_patch_stereo_vertex(e->spv,e->words,&patched,&pc2,
                    lo,ro,conv,/*inj_vi=*/true)) {
                STEREO_LOG("Pipe %u PathB: VS patch failed",p); continue; }
            if (dump) {
                char dp[512];
                _snprintf(dp,sizeof(dp)-1,"%s\\pipe%04d_b_vs.spv",dump,dump_n++);
                FILE *f=fopen(dp,"wb"); if(f){fwrite(patched,4,pc2,f);fclose(f);}
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
            st[vs_stage].module=tmp;
            infos[p].pStages=st; tmp_mod[p]=tmp; tst[p]=st;
            STEREO_LOG("Pipe %u: Path B — VS gl_ViewIndex patch",p);
            continue;
        }

        STEREO_LOG("Pipe %u: no patchable VS/TES stage (stageCount=%u has_vs=%d has_tes=%d has_tcs=%d) — not patched",
                   p, ci->stageCount, has_vs, has_tes, has_tcs);
    }

    VkResult res=sd->real.CreateGraphicsPipelines(sd->real_device,pc,N,infos,pAlloc,pP);
    STEREO_LOG("CreateGraphicsPipelines result=%d",res);

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
