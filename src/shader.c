/*
 * shader.c — SPIR-V stereo injection, deferred to pipeline creation
 *
 * Architecture
 * ────────────
 * Patching at vkCreateShaderModule is wrong for pipelines that contain a
 * geometry or tessellation stage: the geometry shader recomputes gl_Position
 * from scratch, so any offset injected into the vertex shader is silently
 * overwritten before rasterisation.
 *
 * The fix: cache the original (unpatched) SPIR-V at CreateShaderModule
 * time, then at CreateGraphicsPipelines time inspect the stage list, pick
 * the last pre-rasterisation stage (priority: GS > TES > VS), patch only
 * that stage's SPIR-V, create a temporary VkShaderModule from it, build the
 * pipeline, and immediately destroy the temporary module.
 *
 * Stage patching priority
 *   Geometry (GS)              → ExecutionModel 3
 *   Tessellation Eval (TES)    → ExecutionModel 2
 *   Vertex (VS)                → ExecutionModel 0
 *
 * The patcher accepts all three execution models — the gl_Position injection
 * logic (AccessChain, load gl_ViewIndex, select per-eye offset, store) is
 * identical for all three because they all write to Output gl_Position.
 *
 * Geometry-shader bug (fixed here)
 * The Sascha Willems geometryshader sample rendered normal-visualisation
 * lines in stereo (geometry shader passed through the VS-patched position)
 * but the 3D mesh was flat (geometry shader rewrote gl_Position from MVP).
 * Patching the GS instead of the VS fixes both cases correctly.
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
#define SpvOpCompositeExtract 81
#define SpvOpCompositeInsert  82
#define SpvOpFAdd           129
#define SpvOpFMul           133
#define SpvOpIEqual         170
#define SpvOpSelect         169

#define SpvDecorationBuiltIn  11
#define SpvBuiltInPosition     0
#define SpvBuiltInViewIndex 4440

#define SpvStorageClassInput   1
#define SpvStorageClassOutput  3

#define SpvExecVertex      0
#define SpvExecTessEval    2
#define SpvExecGeometry    3

#define SpvCapabilityMultiView 5296

#define SPIRV_MAGIC 0x07230203u

/* ── Dynamic word buffer ──────────────────────────────────────────────────── */
typedef struct { uint32_t *words; size_t count, cap; } SpvBuf;

static bool sb_init(SpvBuf *b, size_t n)
{ b->words=malloc(n*4); b->count=0; b->cap=n; return !!b->words; }

static bool sb_push(SpvBuf *b, uint32_t w) {
    if (b->count>=b->cap) {
        uint32_t *p=realloc(b->words, b->cap*8);
        if (!p) return false; b->words=p; b->cap*=2;
    }
    b->words[b->count++]=w; return true;
}
static bool sb_push_n(SpvBuf *b, const uint32_t *ws, size_t n)
{ for (size_t i=0;i<n;i++) if (!sb_push(b,ws[i])) return false; return true; }
static void sb_free(SpvBuf *b) { free(b->words); b->words=NULL; b->count=b->cap=0; }
static inline uint32_t spv_op(uint32_t op, uint32_t wc) { return (wc<<16)|op; }

/* ── Module scanner ───────────────────────────────────────────────────────── */
typedef struct {
    const uint32_t *words; size_t count;
    uint32_t bound;
    bool     is_patchable;
    bool     has_mv_cap;
    int      exec_model;      /* SpvExecVertex / SpvExecGeometry / … */

    uint32_t pos_var;
    bool     pos_is_block;
    uint32_t pos_block_type, pos_member_idx, pos_ptr_type;

    uint32_t view_var;
    uint32_t float_type, v4_type, int_type, bool_type;
    uint32_t ptr_out_v4, ptr_in_int;
    size_t   fn_word;
} SpvMod;

static void scan_pass(SpvMod *m, bool p2)
{
    const uint32_t *w = m->words;
    for (size_t i=5; i<m->count; ) {
        uint32_t op=w[i]&0xffff, wc=w[i]>>16;
        if (!wc||i+wc>m->count) break;
        if (!p2) switch (op) {
            case SpvOpCapability:
                if (wc>=2&&w[i+1]==SpvCapabilityMultiView) m->has_mv_cap=true; break;
            case SpvOpEntryPoint:
                if (wc>=2) { uint32_t e=w[i+1];
                    if (e==SpvExecVertex||e==SpvExecTessEval||e==SpvExecGeometry)
                        { m->is_patchable=true; m->exec_model=(int)e; } } break;
            case SpvOpTypeFloat:
                if (wc==3&&w[i+2]==32) m->float_type=w[i+1]; break;
            case SpvOpTypeVector:
                if (wc==4&&w[i+2]==m->float_type&&w[i+3]==4) m->v4_type=w[i+1]; break;
            case SpvOpTypeInt:
                if (wc==4&&w[i+2]==32) m->int_type=w[i+1]; break;
            case SpvOpTypeBool:
                if (wc==2) m->bool_type=w[i+1]; break;
            case SpvOpTypePointer:
                if (wc>=4) {
                    if (w[i+2]==SpvStorageClassOutput&&m->v4_type&&w[i+3]==m->v4_type)
                        m->ptr_out_v4=w[i+1];
                    if (w[i+2]==SpvStorageClassInput&&m->int_type&&w[i+3]==m->int_type)
                        m->ptr_in_int=w[i+1];
                } break;
            case SpvOpDecorate:
                if (wc>=4&&w[i+2]==SpvDecorationBuiltIn) {
                    if (w[i+3]==SpvBuiltInPosition&&!m->pos_is_block) m->pos_var=w[i+1];
                    if (w[i+3]==SpvBuiltInViewIndex)                  m->view_var=w[i+1];
                } break;
            case SpvOpMemberDecorate:
                if (wc>=5&&w[i+3]==SpvDecorationBuiltIn&&w[i+4]==SpvBuiltInPosition)
                    { m->pos_block_type=w[i+1]; m->pos_member_idx=w[i+2];
                      m->pos_is_block=true; m->pos_var=0; } break;
            case SpvOpFunction:
                if (!m->fn_word) m->fn_word=i; break;
        } else {
            if (op==SpvOpTypePointer&&wc>=4&&w[i+2]==SpvStorageClassOutput
                &&m->pos_block_type&&w[i+3]==m->pos_block_type) m->pos_ptr_type=w[i+1];
            if (op==SpvOpVariable&&wc>=4&&w[i+3]==SpvStorageClassOutput
                &&m->pos_ptr_type&&w[i+1]==m->pos_ptr_type) m->pos_var=w[i+2];
        }
        i+=wc;
    }
}
static void spirv_scan(SpvMod *m)
{ m->bound=m->words[3]; scan_pass(m,false); if (m->pos_is_block) scan_pass(m,true); }

/* ── Code injection ───────────────────────────────────────────────────────── */
static bool inject(SpvBuf *out, SpvMod *m, uint32_t *nid,
                   float lo, float ro, bool inj_vi, uint32_t *out_iv)
{
    if (!m->float_type||!m->v4_type||!m->pos_var) return false;

    bool will_inj  = inj_vi && !m->view_var && m->int_type;
    bool have_view = m->view_var || will_inj;

    uint32_t pv4  = (*nid)++, pint = (*nid)++, iv  = will_inj?(*nid)++:0;
    uint32_t ch   = (*nid)++, lpos = (*nid)++;
    uint32_t lv   = have_view?(*nid)++:0;
    uint32_t cz   = (*nid)++, isl = have_view?(*nid)++:0;
    uint32_t cl   = (*nid)++, cr  = (*nid)++, sel = (*nid)++;
    uint32_t pw   = (*nid)++, dlt = (*nid)++, px  = (*nid)++;
    uint32_t nx   = (*nid)++, np  = (*nid)++;

    uint32_t uv4  = m->ptr_out_v4 ? m->ptr_out_v4 : pv4;
    uint32_t uint_ = m->ptr_in_int ? m->ptr_in_int : pint;
    uint32_t bt    = m->bool_type, nb = 0;
    if (!bt && have_view && m->int_type) { nb=(*nid)++; bt=nb; }

    /* Type section */
    if (!m->ptr_out_v4) {
        uint32_t w[]={spv_op(SpvOpTypePointer,4),pv4,SpvStorageClassOutput,m->v4_type};
        sb_push_n(out,w,4);
    }
    if (m->int_type && !m->ptr_in_int) {
        uint32_t w[]={spv_op(SpvOpTypePointer,4),pint,SpvStorageClassInput,m->int_type};
        sb_push_n(out,w,4); m->ptr_in_int=pint; uint_=pint;
    }
    if (m->int_type) {
        uint32_t w[]={spv_op(SpvOpConstant,4),m->int_type,cz,0}; sb_push_n(out,w,4); }
    if (nb) { uint32_t w[]={spv_op(SpvOpTypeBool,2),nb}; sb_push_n(out,w,2); }
    { uint32_t w[4]={spv_op(SpvOpConstant,4),m->float_type,cl,0}; memcpy(&w[3],&lo,4); sb_push_n(out,w,4); }
    { uint32_t w[4]={spv_op(SpvOpConstant,4),m->float_type,cr,0}; memcpy(&w[3],&ro,4); sb_push_n(out,w,4); }

    /* Inject gl_ViewIndex variable if absent */
    if (will_inj) {
        uint32_t d[]={spv_op(SpvOpDecorate,4),iv,SpvDecorationBuiltIn,SpvBuiltInViewIndex};
        sb_push_n(out,d,4);
        uint32_t v[]={spv_op(SpvOpVariable,4),uint_,iv,SpvStorageClassInput};
        sb_push_n(out,v,4);
        m->view_var=iv; if (out_iv) *out_iv=iv;
    } else { if (out_iv) *out_iv=0; }

    /* Body: load pos, optionally select per-eye offset, write back */
    uint32_t pptr;
    if (m->pos_is_block) {
        if (!m->int_type) return false;
        uint32_t a[]={spv_op(SpvOpAccessChain,5),uv4,ch,m->pos_var,cz}; sb_push_n(out,a,5);
        pptr=ch;
    } else { pptr=m->pos_var; (void)ch; }

    { uint32_t w[]={spv_op(SpvOpLoad,4),m->v4_type,lpos,pptr}; sb_push_n(out,w,4); }

    if (m->view_var && m->int_type && bt) {
        { uint32_t w[]={spv_op(SpvOpLoad,4),m->int_type,lv,m->view_var}; sb_push_n(out,w,4); }
        { uint32_t w[]={spv_op(SpvOpIEqual,5),bt,isl,lv,cz}; sb_push_n(out,w,5); }
        { uint32_t w[]={spv_op(SpvOpSelect,6),m->float_type,sel,isl,cl,cr}; sb_push_n(out,w,6); }
    } else { sel=cl; (void)lv; (void)isl; }

    { uint32_t w[]={spv_op(SpvOpCompositeExtract,5),m->float_type,pw,lpos,3u}; sb_push_n(out,w,5); }
    { uint32_t w[]={spv_op(SpvOpFMul,5),m->float_type,dlt,sel,pw}; sb_push_n(out,w,5); }
    { uint32_t w[]={spv_op(SpvOpCompositeExtract,5),m->float_type,px,lpos,0u}; sb_push_n(out,w,5); }
    { uint32_t w[]={spv_op(SpvOpFAdd,5),m->float_type,nx,px,dlt}; sb_push_n(out,w,5); }
    { uint32_t w[]={spv_op(SpvOpCompositeInsert,6),m->v4_type,np,nx,lpos,0u}; sb_push_n(out,w,6); }
    { uint32_t w[]={spv_op(SpvOpStore,3),pptr,np}; sb_push_n(out,w,3); }
    return true;
}

/* ── SPIR-V patcher ───────────────────────────────────────────────────────── */
bool spirv_patch_stereo_vertex(const uint32_t *in, size_t in_c,
                               uint32_t **out, size_t *out_c,
                               float lo, float ro, float conv, bool inj_vi)
{
    (void)conv;
    if (!in||in_c<5||in[0]!=SPIRV_MAGIC) return false;

    SpvMod m={0}; m.words=in; m.count=in_c;
    spirv_scan(&m);

    if (!m.is_patchable) { STEREO_LOG("Shader: not patchable (model=%d)", m.exec_model); return false; }
    if (!m.pos_var)       { STEREO_LOG("Shader: no gl_Position (model=%d)", m.exec_model); return false; }
    STEREO_LOG("Shader: model=%d block=%d pos=%u view=%u bound=%u",
               m.exec_model,(int)m.pos_is_block,m.pos_var,m.view_var,m.bound);

    SpvBuf comb; if (!sb_init(&comb,256)) return false;
    uint32_t nid=m.bound, iv=0;
    if (!inject(&comb,&m,&nid,lo,ro,inj_vi,&iv)) { sb_free(&comb); return false; }

    /* Split combined into type-extras and body-extras */
    SpvBuf te,be;
    if (!sb_init(&te,64)||!sb_init(&be,64)) { sb_free(&comb);sb_free(&te);sb_free(&be); return false; }
    size_t split=0;
    for (size_t i=0; i<comb.count; ) {
        uint32_t op=comb.words[i]&0xffff;
        if (op==SpvOpLoad||op==SpvOpStore||op==SpvOpAccessChain||
            op==SpvOpIEqual||op==SpvOpSelect||op==SpvOpFMul||op==SpvOpFAdd||
            op==SpvOpCompositeExtract||op==SpvOpCompositeInsert) { split=i; break; }
        uint32_t wc=comb.words[i]>>16; if (!wc) break; i+=wc; split=i;
    }
    for (size_t i=0;i<split;i++) sb_push(&te,comb.words[i]);
    for (size_t i=split;i<comb.count;i++) sb_push(&be,comb.words[i]);
    sb_free(&comb);

    /* Find insertion points in original binary */
    size_t ins_t=0, ins_b=0;
    for (size_t i=5; i<in_c; ) {
        uint32_t op=in[i]&0xffff, wc=in[i]>>16;
        if (!wc||i+wc>in_c) break;
        if (op==SpvOpFunction&&!ins_t) ins_t=i;
        if (!m.pos_is_block&&op==SpvOpStore&&wc>=3&&in[i+1]==m.pos_var) ins_b=i+wc;
        i+=wc;
    }
    if (!ins_t) { sb_free(&te);sb_free(&be); return false; }
    if (!ins_b) {
        for (size_t i=5; i<in_c; ) {
            uint32_t op=in[i]&0xffff, wc=in[i]>>16; if (!wc) break;
            if (op==253||op==254) { ins_b=i; break; } i+=wc;
        }
        if (!ins_b) { sb_free(&te);sb_free(&be); return false; }
    }
    if (ins_b<ins_t) { sb_free(&te);sb_free(&be); return false; }

    bool need_mv=iv&&!m.has_mv_cap, mv_done=false;
    SpvBuf ob; if (!sb_init(&ob, in_c+te.count+be.count+16)) { sb_free(&te);sb_free(&be); return false; }
    sb_push_n(&ob,in,5); ob.words[3]=nid;

    for (size_t i=5; i<in_c; ) {
        if (!mv_done&&need_mv) {
            uint32_t c[]={spv_op(SpvOpCapability,2),SpvCapabilityMultiView};
            sb_push_n(&ob,c,2); mv_done=true;
        }
        if (i==ins_t&&te.count) { sb_push_n(&ob,te.words,te.count); te.count=0; }
        if (i==ins_b&&be.count) { sb_push_n(&ob,be.words,be.count); be.count=0; }

        uint32_t op=in[i]&0xffff, wc=in[i]>>16;
        if (!wc||i+wc>in_c) break;

        /* Extend OpEntryPoint interface list with the injected view variable */
        if (op==SpvOpEntryPoint&&wc>=4&&iv!=0&&
            (in[i+1]==SpvExecVertex||in[i+1]==SpvExecGeometry||in[i+1]==SpvExecTessEval)) {
            sb_push(&ob,((wc+1)<<16)|SpvOpEntryPoint);
            sb_push_n(&ob,&in[i+1],wc-1);
            sb_push(&ob,iv);
            STEREO_LOG("OpEntryPoint extended: view_var=%u exec=%u", iv, in[i+1]);
            i+=wc; continue;
        }
        sb_push_n(&ob,&in[i],wc); i+=wc;
    }
    if (te.count) sb_push_n(&ob,te.words,te.count);
    if (be.count) sb_push_n(&ob,be.words,be.count);
    sb_free(&te); sb_free(&be);

    *out=ob.words; *out_c=ob.count;
    STEREO_LOG("Patched: model=%d  %zu→%zu words  bound=%u  vi=%d  mvcap=%d",
               m.exec_model,in_c,ob.count,nid,(int)(iv!=0),(int)mv_done);
    return true;
}
void spirv_patched_free(uint32_t *w) { free(w); }

/* ── Shader module cache ──────────────────────────────────────────────────── */

/* Quick check: is this SPIR-V a patchable stage? Avoids caching FS/CS. */
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
        STEREO_ERR("Shader cache full, cannot cache %p", (void*)(uintptr_t)h);
        return;
    }
    uint32_t *cp=malloc(words*4); if (!cp) return;
    memcpy(cp,spv,words*4);
    StereoShaderCache *e=&sd->shader_cache[sd->shader_cache_count++];
    e->handle=h; e->spv=cp; e->words=words;
    STEREO_LOG("cache_add: module=%p words=%zu slot=%u",
               (void*)(uintptr_t)h, words, sd->shader_cache_count-1);
}
static void cache_remove(StereoDevice *sd, VkShaderModule h)
{
    for (uint32_t i=0;i<sd->shader_cache_count;i++) {
        if (sd->shader_cache[i].handle==h) {
            free(sd->shader_cache[i].spv);
            sd->shader_cache[i]=sd->shader_cache[--sd->shader_cache_count];
            return;
        }
    }
}

/* ── vkCreateShaderModule: cache, do NOT patch ───────────────────────────── */
VKAPI_ATTR VkResult VKAPI_CALL
stereo_CreateShaderModule(
    VkDevice                         device,
    const VkShaderModuleCreateInfo  *pCreateInfo,
    const VkAllocationCallbacks     *pAllocator,
    VkShaderModule                  *pShaderModule)
{
    StereoDevice *sd = stereo_device_from_handle(device);
    if (!sd) return VK_ERROR_DEVICE_LOST;

    /* Always pass original SPIR-V straight through to the driver */
    VkResult res = sd->real.CreateShaderModule(
        sd->real_device, pCreateInfo, pAllocator, pShaderModule);
    if (res!=VK_SUCCESS) return res;

    if (!sd->stereo.enabled) return VK_SUCCESS;

    const uint32_t *spv = (const uint32_t*)pCreateInfo->pCode;
    size_t          wc  = pCreateInfo->codeSize/4;

    if (is_patchable_spv(spv, wc))
        cache_add(sd, *pShaderModule, spv, wc);

    return VK_SUCCESS;
}

/* ── vkCreateGraphicsPipelines: patch last pre-rast stage ───────────────── */
VKAPI_ATTR VkResult VKAPI_CALL
stereo_CreateGraphicsPipelines(
    VkDevice                            device,
    VkPipelineCache                     pipelineCache,
    uint32_t                            N,
    const VkGraphicsPipelineCreateInfo *pCreateInfos,
    const VkAllocationCallbacks        *pAllocator,
    VkPipeline                         *pPipelines)
{
    StereoDevice *sd = stereo_device_from_handle(device);
    if (!sd) return VK_ERROR_DEVICE_LOST;
    STEREO_LOG("CreateGraphicsPipelines: count=%u stereo=%d", N, sd->stereo.enabled);

    if (!sd->stereo.enabled) {
        return sd->real.CreateGraphicsPipelines(
            sd->real_device, pipelineCache, N, pCreateInfos, pAllocator, pPipelines);
    }

    VkShaderModule               *tmods   = calloc(N, sizeof(VkShaderModule));
    VkPipelineShaderStageCreateInfo **tstages = calloc(N, sizeof(void*));
    VkGraphicsPipelineCreateInfo    *infos    = malloc(N*sizeof(*infos));
    if (!tmods||!tstages||!infos) {
        free(tmods);free(tstages);free(infos); return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    memcpy(infos, pCreateInfos, N*sizeof(*infos));

    const char *dump_dir = stereo_getenv("VKS3D_DUMP_SPIRV");
    static int dump_n = 0;

    for (uint32_t p=0; p<N; p++) {
        const VkGraphicsPipelineCreateInfo *ci = &pCreateInfos[p];

        /* Find last pre-rasterisation stage: GS(2) > TES(1) > VS(0) */
        int best=-1; uint32_t best_s=~0u;
        for (uint32_t s=0; s<ci->stageCount; s++) {
            VkShaderStageFlagBits sf=ci->pStages[s].stage;
            int pr=(sf==VK_SHADER_STAGE_GEOMETRY_BIT)               ? 2 :
                   (sf==VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT) ? 1 :
                   (sf==VK_SHADER_STAGE_VERTEX_BIT)                  ? 0 : -1;
            if (pr>best) { best=pr; best_s=s; }
        }
        if (best_s==~0u) continue;

        STEREO_LOG("Pipeline %u: last pre-rast stage=%u (prio=%d) module=%p",
                   p, best_s, best, (void*)(uintptr_t)ci->pStages[best_s].module);

        StereoShaderCache *entry=cache_find(sd, ci->pStages[best_s].module);
        if (!entry) { STEREO_LOG("Pipeline %u: not in cache, no patch", p); continue; }

        uint32_t *patched=NULL; size_t pc=0;
        if (!spirv_patch_stereo_vertex(
                entry->spv, entry->words, &patched, &pc,
                sd->stereo.left_eye_offset, sd->stereo.right_eye_offset,
                sd->stereo.convergence, sd->stereo.multiview)) {
            STEREO_LOG("Pipeline %u: patch failed", p); continue;
        }

        if (dump_dir) {
            char path[512];
            _snprintf(path,sizeof(path)-1,"%s\\pipe%04d_s%u.spv",dump_dir,dump_n++,best_s);
            FILE *f=fopen(path,"wb");
            if (f) { fwrite(patched,4,pc,f); fclose(f);
                     STEREO_LOG("Dumped: %s", path); }
        }

        VkShaderModuleCreateInfo smci={
            .sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
            .codeSize=pc*4, .pCode=patched };
        VkShaderModule tmp=VK_NULL_HANDLE;
        VkResult mr=sd->real.CreateShaderModule(sd->real_device,&smci,NULL,&tmp);
        spirv_patched_free(patched);
        if (mr!=VK_SUCCESS) { STEREO_ERR("Pipeline %u: tmp module failed %d",p,mr); continue; }

        uint32_t sc=ci->stageCount;
        VkPipelineShaderStageCreateInfo *stages=malloc(sc*sizeof(*stages));
        if (!stages) { sd->real.DestroyShaderModule(sd->real_device,tmp,NULL); continue; }
        memcpy(stages,ci->pStages,sc*sizeof(*stages));
        stages[best_s].module=tmp;

        infos[p].pStages=stages;
        tmods[p]=tmp; tstages[p]=stages;
        STEREO_LOG("Pipeline %u: patched stage %u with tmp=%p",p,best_s,(void*)(uintptr_t)tmp);
    }

    VkResult res=sd->real.CreateGraphicsPipelines(
        sd->real_device,pipelineCache,N,infos,pAllocator,pPipelines);
    STEREO_LOG("CreateGraphicsPipelines result=%d", res);

    for (uint32_t p=0;p<N;p++) {
        if (tmods[p]) sd->real.DestroyShaderModule(sd->real_device,tmods[p],NULL);
        free(tstages[p]);
    }
    free(tmods); free(tstages); free(infos);
    return res;
}

/* ── vkDestroyShaderModule ───────────────────────────────────────────────── */
VKAPI_ATTR void VKAPI_CALL
stereo_DestroyShaderModule(VkDevice device, VkShaderModule sm,
                           const VkAllocationCallbacks *pAllocator)
{
    StereoDevice *sd=stereo_device_from_handle(device);
    if (!sd) return;
    cache_remove(sd, sm);
    sd->real.DestroyShaderModule(sd->real_device, sm, pAllocator);
}
