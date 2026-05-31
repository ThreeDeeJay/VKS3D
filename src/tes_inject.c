/*
 * tes_inject.c — TCS + TES SPIR-V builders with full user-varying passthrough
 *
 * Problem solved: when we inject TCS+TES between VS and FS (or VS and GS),
 * the TES becomes the last pre-rasterization stage.  The downstream stage
 * (FS or GS) still expects to read the same user-defined varyings the VS
 * outputs (Location 0, 1, 2, …).  If TES only outputs gl_Position, the
 * driver crashes at CreateGraphicsPipelines with a TES→FS interface mismatch.
 *
 * Fix: scan VS SPIR-V for user-defined Output variables (Location-decorated,
 * float vector types) and generate TCS/TES that barycentrically pass them
 * all through, preserving Location numbers so the FS/GS finds what it expects.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "stereo_icd.h"
#include "tes_inject.h"

/* ── SPIR-V word buffer ───────────────────────────────────────────────────── */
typedef struct { uint32_t *w; size_t n, cap; } SB;
static bool sb_i(SB *b, size_t c) { b->w=malloc(c*4); b->n=0; b->cap=c; return !!b->w; }
static bool sb_p(SB *b, uint32_t v) {
    if (b->n>=b->cap){uint32_t *p=realloc(b->w,b->cap*8);if(!p)return false;b->w=p;b->cap*=2;}
    b->w[b->n++]=v; return true; }
static bool sb_pn(SB *b, const uint32_t *v, size_t c) {
    for(size_t i=0;i<c;i++) if(!sb_p(b,v[i])) return false; return true; }
static uint32_t OP(uint32_t op, uint32_t wc) { return (wc<<16)|op; }

/* ── User varying descriptor ─────────────────────────────────────────────── */
typedef struct { uint32_t location; uint32_t vec_size; } UserVar;
#define MAX_USER_VARS 16

/* ── Scan VS block (gl_PerVertex) ─────────────────────────────────────────── */
#define MAX_MBRS 8
typedef enum { MBR_VEC4=0, MBR_FLOAT=1, MBR_FARR=2 } MbrKind;
typedef struct { MbrKind kind; uint32_t arr_n; uint32_t builtin; } Mbr;

static int scan_vs_block(const uint32_t *spv, size_t wc, Mbr out[MAX_MBRS])
{
    uint32_t block_id = 0;
    for (size_t i=5; i<wc; ) {
        uint32_t op=spv[i]&0xffff, n=spv[i]>>16;
        if (!n||i+n>wc) break;
        if (op==72&&n>=5&&spv[i+2]==0&&spv[i+3]==11&&spv[i+4]==0) block_id=spv[i+1];
        i+=n;
    }
    if (!block_id) return 0;

    uint32_t mbr_types[MAX_MBRS]={0}; int n_mbrs=0;
    for (size_t i=5; i<wc; ) {
        uint32_t op=spv[i]&0xffff, n=spv[i]>>16;
        if (!n||i+n>wc) break;
        if (op==30&&n>=2&&spv[i+1]==block_id) {
            n_mbrs=(int)(n-2); if(n_mbrs>MAX_MBRS) n_mbrs=MAX_MBRS;
            for(int m=0;m<n_mbrs;m++) mbr_types[m]=spv[i+2+m];
        }
        i+=n;
    }
    if (!n_mbrs) return 0;

    uint32_t float_id=0;
    for (size_t i=5; i<wc; ) {
        uint32_t op=spv[i]&0xffff, n=spv[i]>>16;
        if (!n||i+n>wc) break;
        if (op==22&&n>=3&&spv[i+2]==32) float_id=spv[i+1];
        i+=n;
    }

    for (int m=0; m<n_mbrs; m++) {
        uint32_t tid=mbr_types[m];
        out[m].kind=MBR_FLOAT; out[m].arr_n=0; out[m].builtin=0;
        for (size_t i=5; i<wc; ) {
            uint32_t op=spv[i]&0xffff, n=spv[i]>>16;
            if (!n||i+n>wc) break;
            if (spv[i+1]==tid) {
                if (op==22) { out[m].kind=MBR_FLOAT; }
                else if (op==23&&n>=4) { out[m].kind=(spv[i+2]==float_id&&spv[i+3]==4)?MBR_VEC4:MBR_FLOAT; }
                else if (op==28&&n>=4) {
                    out[m].kind=MBR_FARR;
                    uint32_t lid=spv[i+3];
                    for(size_t j=5;j<wc;){uint32_t o2=spv[j]&0xffff,n2=spv[j]>>16;if(!n2||j+n2>wc)break;
                        if(o2==43&&n2>=4&&spv[j+2]==lid)out[m].arr_n=spv[j+3];j+=n2;}
                    if(!out[m].arr_n) out[m].arr_n=1;
                }
                break;
            }
            i+=n;
        }
    }
    for (size_t i=5; i<wc; ) {
        uint32_t op=spv[i]&0xffff, n=spv[i]>>16;
        if (!n||i+n>wc) break;
        if (op==72&&n>=5&&spv[i+1]==block_id&&spv[i+3]==11) {
            uint32_t mem=spv[i+2]; if(mem<(uint32_t)n_mbrs) out[mem].builtin=spv[i+4];
        }
        i+=n;
    }
    if (n_mbrs>=1) out[0].kind=MBR_VEC4;
    return n_mbrs;
}

/* ── Scan VS user-defined Output varyings ─────────────────────────────────── */
static int scan_vs_user_vars(const uint32_t *spv, size_t wc, UserVar vars[MAX_USER_VARS])
{
    if (!spv||wc<5) return 0;
    uint32_t bound=spv[3];
    if (bound<4||bound>65536) return 0;

    uint32_t *var_loc = (uint32_t*)calloc(bound,sizeof(uint32_t));
    uint8_t  *has_loc = (uint8_t*) calloc(bound,1);
    uint8_t  *is_bi   = (uint8_t*) calloc(bound,1);
    uint8_t  *is_out  = (uint8_t*) calloc(bound,1);
    uint8_t  *is_blk  = (uint8_t*) calloc(bound,1);
    uint32_t *var_tp  = (uint32_t*)calloc(bound,sizeof(uint32_t));
    uint32_t *ptr_el  = (uint32_t*)calloc(bound,sizeof(uint32_t));
    uint32_t *vec_sz  = (uint32_t*)calloc(bound,sizeof(uint32_t));

    if (!var_loc||!has_loc||!is_bi||!is_out||!is_blk||!var_tp||!ptr_el||!vec_sz) {
        free(var_loc);free(has_loc);free(is_bi);free(is_out);
        free(is_blk);free(var_tp);free(ptr_el);free(vec_sz); return 0;
    }

    uint32_t float_id=0;
    for (size_t i=5; i<wc; ) {
        uint32_t op=spv[i]&0xffff, n=spv[i]>>16;
        if (!n||i+n>wc) break;
        if (op==22&&n>=3&&spv[i+2]==32&&spv[i+1]<bound){float_id=spv[i+1];vec_sz[float_id]=1;}
        i+=n;
    }
    for (size_t i=5; i<wc; ) {
        uint32_t op=spv[i]&0xffff, n=spv[i]>>16;
        if (!n||i+n>wc) break;
        switch(op) {
        case 23: /* OpTypeVector */
            if(n>=4&&spv[i+1]<bound&&float_id&&spv[i+2]==float_id&&spv[i+3]>=2&&spv[i+3]<=4)
                vec_sz[spv[i+1]]=spv[i+3]; break;
        case 32: /* OpTypePointer Output */
            if(n>=4&&spv[i+1]<bound&&spv[i+2]==3&&spv[i+3]<bound)
                ptr_el[spv[i+1]]=spv[i+3]; break;
        case 59: /* OpVariable */
            if(n>=4&&spv[i+1]<bound&&spv[i+2]<bound){
                var_tp[spv[i+2]]=spv[i+1];
                if(spv[i+3]==3) is_out[spv[i+2]]=1;
            } break;
        case 71: /* OpDecorate */
            if(n>=3&&spv[i+1]<bound){
                if(spv[i+2]==30&&n>=4){has_loc[spv[i+1]]=1;var_loc[spv[i+1]]=spv[i+3];}
                if(spv[i+2]==11) is_bi[spv[i+1]]=1;
                if(spv[i+2]==2)  is_blk[spv[i+1]]=1;
            } break;
        }
        i+=n;
    }
    /* Mark block element types */
    for (uint32_t id=1; id<bound; id++) {
        if (!is_out[id]||!var_tp[id]) continue;
        uint32_t el=ptr_el[var_tp[id]];
        if (el&&el<bound&&is_blk[el]) is_bi[id]=1; /* treat Block vars as builtin */
    }

    int count=0;
    for (uint32_t id=1; id<bound&&count<MAX_USER_VARS; id++) {
        if (!has_loc[id]||!is_out[id]||is_bi[id]) continue;
        uint32_t tp=var_tp[id]; if(!tp||tp>=bound) continue;
        uint32_t el=ptr_el[tp]; if(!el||el>=bound) continue;
        uint32_t vs=vec_sz[el]; if(!vs||vs>4) continue;
        vars[count].location=var_loc[id]; vars[count].vec_size=vs; count++;
    }
    /* Sort by location */
    for(int a=0;a<count-1;a++) for(int b=a+1;b<count;b++)
        if(vars[b].location<vars[a].location){UserVar t=vars[a];vars[a]=vars[b];vars[b]=t;}

    free(var_loc);free(has_loc);free(is_bi);free(is_out);
    free(is_blk);free(var_tp);free(ptr_el);free(vec_sz);

    for(int i=0;i<count;i++)
        STEREO_LOG("  user_var[%d] loc=%u vsz=%u",i,vars[i].location,vars[i].vec_size);
    return count;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * TCS builder — pass-through, 3 control points, tessLevel=1
 * Passes gl_Position AND all user-defined varyings from VS.
 * ═══════════════════════════════════════════════════════════════════════════ */
bool build_tcs_spv(const uint32_t *vs_spv, size_t vs_wc,
                   uint32_t **out, size_t *out_c)
{
    Mbr mbrs[MAX_MBRS]; int n_mbrs=vs_spv?scan_vs_block(vs_spv,vs_wc,mbrs):0;
    if(n_mbrs<1){n_mbrs=1;mbrs[0].kind=MBR_VEC4;mbrs[0].arr_n=0;mbrs[0].builtin=0;}

    UserVar uvars[MAX_USER_VARS]; int n_uvars=vs_spv?scan_vs_user_vars(vs_spv,vs_wc,uvars):0;
    STEREO_LOG("build_tcs_spv: %d block mbrs, %d user vars",n_mbrs,n_uvars);

    bool need_vs[5]={false};
    for(int u=0;u<n_uvars;u++) need_vs[uvars[u].vec_size]=true;

    /* ── ID allocation ─────────────────────────────────────────────────── */
    uint32_t nid=1;
    uint32_t id_void=nid++,id_int=nid++,id_uint=nid++,id_float=nid++,id_vec4=nid++;

    /* FARR block member constants/types */
    uint32_t id_fc[MAX_MBRS]={0},id_ft[MAX_MBRS]={0};
    for(int m=0;m<n_mbrs;m++){
        id_fc[m]=(mbrs[m].kind==MBR_FARR)?nid++:0;
        id_ft[m]=(mbrs[m].kind==MBR_FARR)?nid++:0;
    }

    uint32_t id_c32=nid++,id_c3=nid++,id_c2=nid++,id_c4=nid++;
    uint32_t id_pv_in=nid++,id_pv_out=nid++;
    uint32_t id_arr_in=nid++,id_arr_out=nid++;
    uint32_t id_fla2=nid++,id_fla4=nid++;
    uint32_t id_pIarr=nid++,id_pOarr=nid++;
    uint32_t id_pIv4=nid++,id_pOv4=nid++;   /* ptr Input/Output vec4 (for block Position) */
    uint32_t id_pIint=nid++;
    uint32_t id_pOf2=nid++,id_pOf4=nid++,id_pOf=nid++;  /* ptr Output float[2], float[4], float */
    uint32_t id_gl_in=nid++,id_gl_out=nid++;
    uint32_t id_tl_i=nid++,id_tl_o=nid++,id_invoc=nid++;
    uint32_t id_i0=nid++,id_i1=nid++,id_i2=nid++,id_i3=nid++;
    uint32_t id_f1=nid++;
    uint32_t id_fnty=nid++,id_main=nid++,id_label=nid++;
    uint32_t id_inv=nid++,id_inptr=nid++,id_pos=nid++,id_optr=nid++;
    uint32_t id_tl[6]; for(int k=0;k<6;k++) id_tl[k]=nid++;

    /* User varying vector types (vec2, vec3 are new; vec4/float already declared) */
    uint32_t id_uvec[5]={0};
    id_uvec[1]=id_float; id_uvec[4]=id_vec4;
    if(need_vs[2]) id_uvec[2]=nid++;
    if(need_vs[3]) id_uvec[3]=nid++;

    /* User varying array types: array(T,32) for input, array(T,3) for output */
    uint32_t id_uarr32[5]={0},id_uarr3[5]={0};
    for(int vs=1;vs<=4;vs++) if(need_vs[vs]){id_uarr32[vs]=nid++;id_uarr3[vs]=nid++;}

    /* User varying pointer types.
     * Reuse existing: pIv4 for vs=4 input elem, pOv4 for vs=4 output elem, pOf for vs=1 output elem.
     * New: input elem for vs=1,2,3; output elem for vs=2,3; all array ptrs. */
    uint32_t id_pI_uvec[5]={0},id_pO_uvec[5]={0};   /* elem ptrs (AccessChain result / Output var) */
    uint32_t id_pI_uarr32[5]={0},id_pO_uarr3[5]={0}; /* array var ptrs */
    id_pI_uvec[4]=id_pIv4;  /* REUSE ptr Input vec4 */
    id_pO_uvec[4]=id_pOv4;  /* REUSE ptr Output vec4 */
    id_pO_uvec[1]=id_pOf;   /* REUSE ptr Output float */
    for(int vs=1;vs<=4;vs++){
        if(!need_vs[vs]) continue;
        if(vs!=4) id_pI_uvec[vs]=nid++;          /* new ptr Input T (for vs=1,2,3) */
        if(vs!=4&&vs!=1) id_pO_uvec[vs]=nid++;   /* new ptr Output T (for vs=2,3) */
        id_pI_uarr32[vs]=nid++;
        id_pO_uarr3[vs]=nid++;
    }

    /* Per-user-var variable IDs */
    uint32_t id_uvi[MAX_USER_VARS]={0},id_uvo[MAX_USER_VARS]={0};
    for(int u=0;u<n_uvars;u++){id_uvi[u]=nid++;id_uvo[u]=nid++;}

    /* Body SSA per user var: tmp_in, val, tmp_out */
    uint32_t id_uti[MAX_USER_VARS]={0},id_uvl[MAX_USER_VARS]={0},id_uto[MAX_USER_VARS]={0};
    for(int u=0;u<n_uvars;u++){id_uti[u]=nid++;id_uvl[u]=nid++;id_uto[u]=nid++;}

    uint32_t bound=nid;

    /* ── Emit SPIR-V ───────────────────────────────────────────────────── */
    SB b; if(!sb_i(&b,600+n_mbrs*10+n_uvars*40)) return false;

    /* Header */
    sb_p(&b,0x07230203u);sb_p(&b,0x00010000u);sb_p(&b,0x56533344u);sb_p(&b,bound);sb_p(&b,0u);

    /* Capabilities */
    {uint32_t w[]={OP(17,2),1};sb_pn(&b,w,2);}
    {uint32_t w[]={OP(17,2),34};sb_pn(&b,w,2);}

    /* MemoryModel */
    {uint32_t w[]={OP(14,3),0,1};sb_pn(&b,w,3);}

    /* EntryPoint: TCS(1) main "main" gl_in gl_out tl_i tl_o invoc [user in/out ...] */
    {
        uint32_t ifc=5+(uint32_t)(n_uvars*2);
        sb_p(&b,OP(15,5+ifc)); sb_p(&b,1); sb_p(&b,id_main);
        sb_p(&b,0x6E69616Du); sb_p(&b,0x00000000u);
        sb_p(&b,id_gl_in);sb_p(&b,id_gl_out);sb_p(&b,id_tl_i);sb_p(&b,id_tl_o);sb_p(&b,id_invoc);
        for(int u=0;u<n_uvars;u++){sb_p(&b,id_uvi[u]);sb_p(&b,id_uvo[u]);}
    }
    /* ExecutionMode OutputVertices 3 */
    {uint32_t w[]={OP(16,4),id_main,26,3};sb_pn(&b,w,4);}

    /* ── Decorations ── */
    {uint32_t w[]={OP(71,3),id_pv_in,2};sb_pn(&b,w,3);}  /* Block */
    for(int m=0;m<n_mbrs;m++){
        uint32_t w[]={OP(72,5),id_pv_in,(uint32_t)m,11,mbrs[m].builtin};sb_pn(&b,w,5);}
    {uint32_t w[]={OP(71,3),id_pv_out,2};sb_pn(&b,w,3);}
    {uint32_t w[]={OP(72,5),id_pv_out,0,11,0};sb_pn(&b,w,5);}
    {uint32_t w[]={OP(71,4),id_tl_i,11,12};sb_pn(&b,w,4);}
    {uint32_t w[]={OP(71,3),id_tl_i,15};sb_pn(&b,w,3);}
    {uint32_t w[]={OP(71,4),id_tl_o,11,11};sb_pn(&b,w,4);}
    {uint32_t w[]={OP(71,3),id_tl_o,15};sb_pn(&b,w,3);}
    {uint32_t w[]={OP(71,4),id_invoc,11,8};sb_pn(&b,w,4);}
    for(int u=0;u<n_uvars;u++){
        {uint32_t w[]={OP(71,4),id_uvi[u],30,uvars[u].location};sb_pn(&b,w,4);}
        {uint32_t w[]={OP(71,4),id_uvo[u],30,uvars[u].location};sb_pn(&b,w,4);}
    }

    /* ── Types ── */
    {uint32_t w[]={OP(19,2),id_void};sb_pn(&b,w,2);}
    {uint32_t w[]={OP(21,4),id_int,32,1};sb_pn(&b,w,4);}
    {uint32_t w[]={OP(21,4),id_uint,32,0};sb_pn(&b,w,4);}
    {uint32_t w[]={OP(22,3),id_float,32};sb_pn(&b,w,3);}
    {uint32_t w[]={OP(23,4),id_vec4,id_float,4};sb_pn(&b,w,4);}
    if(need_vs[2]&&id_uvec[2]){uint32_t w[]={OP(23,4),id_uvec[2],id_float,2};sb_pn(&b,w,4);}
    if(need_vs[3]&&id_uvec[3]){uint32_t w[]={OP(23,4),id_uvec[3],id_float,3};sb_pn(&b,w,4);}

    /* FARR block member types */
    for(int m=0;m<n_mbrs;m++) if(mbrs[m].kind==MBR_FARR){
        {uint32_t w[]={OP(43,4),id_uint,id_fc[m],mbrs[m].arr_n};sb_pn(&b,w,4);}
        {uint32_t w[]={OP(28,4),id_ft[m],id_float,id_fc[m]};sb_pn(&b,w,4);}
    }

    /* Array size constants */
    {uint32_t w[]={OP(43,4),id_uint,id_c32,32};sb_pn(&b,w,4);}
    {uint32_t w[]={OP(43,4),id_uint,id_c3,3};sb_pn(&b,w,4);}
    {uint32_t w[]={OP(43,4),id_uint,id_c2,2};sb_pn(&b,w,4);}
    {uint32_t w[]={OP(43,4),id_uint,id_c4,4};sb_pn(&b,w,4);}

    /* Block struct types */
    {
        uint32_t hdr=OP(30,2+(uint32_t)n_mbrs);sb_p(&b,hdr);sb_p(&b,id_pv_in);
        for(int m=0;m<n_mbrs;m++){
            uint32_t mt=(mbrs[m].kind==MBR_VEC4)?id_vec4:(mbrs[m].kind==MBR_FLOAT)?id_float:id_ft[m];
            sb_p(&b,mt);
        }
    }
    {uint32_t w[]={OP(30,3),id_pv_out,id_vec4};sb_pn(&b,w,3);}

    /* Block array types */
    {uint32_t w[]={OP(28,4),id_arr_in,id_pv_in,id_c32};sb_pn(&b,w,4);}
    {uint32_t w[]={OP(28,4),id_arr_out,id_pv_out,id_c3};sb_pn(&b,w,4);}
    {uint32_t w[]={OP(28,4),id_fla2,id_float,id_c2};sb_pn(&b,w,4);}
    {uint32_t w[]={OP(28,4),id_fla4,id_float,id_c4};sb_pn(&b,w,4);}

    /* User varying array types */
    for(int vs=1;vs<=4;vs++) if(need_vs[vs]){
        uint32_t tv=id_uvec[vs];
        {uint32_t w[]={OP(28,4),id_uarr32[vs],tv,id_c32};sb_pn(&b,w,4);}
        {uint32_t w[]={OP(28,4),id_uarr3[vs],tv,id_c3};sb_pn(&b,w,4);}
    }

    /* Block pointer types */
    {uint32_t w[]={OP(32,4),id_pIarr,1,id_arr_in};sb_pn(&b,w,4);}
    {uint32_t w[]={OP(32,4),id_pOarr,3,id_arr_out};sb_pn(&b,w,4);}
    {uint32_t w[]={OP(32,4),id_pIv4,1,id_vec4};sb_pn(&b,w,4);}
    {uint32_t w[]={OP(32,4),id_pOv4,3,id_vec4};sb_pn(&b,w,4);}
    {uint32_t w[]={OP(32,4),id_pIint,1,id_int};sb_pn(&b,w,4);}
    {uint32_t w[]={OP(32,4),id_pOf2,3,id_fla2};sb_pn(&b,w,4);}
    {uint32_t w[]={OP(32,4),id_pOf4,3,id_fla4};sb_pn(&b,w,4);}
    {uint32_t w[]={OP(32,4),id_pOf,3,id_float};sb_pn(&b,w,4);}

    /* User varying pointer types (only emit NEW ones; reused ones already declared) */
    for(int vs=1;vs<=4;vs++){
        if(!need_vs[vs]) continue;
        uint32_t tv=id_uvec[vs];
        /* Input element ptr: new for vs=1,2,3 */
        if(vs!=4&&id_pI_uvec[vs]){
            uint32_t w[]={OP(32,4),id_pI_uvec[vs],1,tv};sb_pn(&b,w,4);}
        /* Output element ptr: new for vs=2,3 (vs=4 reuses pOv4, vs=1 reuses pOf) */
        if(vs!=4&&vs!=1&&id_pO_uvec[vs]){
            uint32_t w[]={OP(32,4),id_pO_uvec[vs],3,tv};sb_pn(&b,w,4);}
        /* Array ptr types: always new */
        {uint32_t w[]={OP(32,4),id_pI_uarr32[vs],1,id_uarr32[vs]};sb_pn(&b,w,4);}
        {uint32_t w[]={OP(32,4),id_pO_uarr3[vs],3,id_uarr3[vs]};sb_pn(&b,w,4);}
    }

    /* ── Variables ── */
    {uint32_t w[]={OP(59,4),id_pIarr,id_gl_in,1};sb_pn(&b,w,4);}
    {uint32_t w[]={OP(59,4),id_pOarr,id_gl_out,3};sb_pn(&b,w,4);}
    {uint32_t w[]={OP(59,4),id_pOf2,id_tl_i,3};sb_pn(&b,w,4);}
    {uint32_t w[]={OP(59,4),id_pOf4,id_tl_o,3};sb_pn(&b,w,4);}
    {uint32_t w[]={OP(59,4),id_pIint,id_invoc,1};sb_pn(&b,w,4);}
    for(int u=0;u<n_uvars;u++){
        uint32_t vs=uvars[u].vec_size;
        {uint32_t w[]={OP(59,4),id_pI_uarr32[vs],id_uvi[u],1};sb_pn(&b,w,4);}
        {uint32_t w[]={OP(59,4),id_pO_uarr3[vs],id_uvo[u],3};sb_pn(&b,w,4);}
    }

    /* ── Constants ── */
    {uint32_t w[]={OP(43,4),id_int,id_i0,0};sb_pn(&b,w,4);}
    {uint32_t w[]={OP(43,4),id_int,id_i1,1};sb_pn(&b,w,4);}
    {uint32_t w[]={OP(43,4),id_int,id_i2,2};sb_pn(&b,w,4);}
    {uint32_t w[]={OP(43,4),id_int,id_i3,3};sb_pn(&b,w,4);}
    {uint32_t w[]={OP(43,4),id_float,id_f1,0x3F800000u};sb_pn(&b,w,4);}

    /* ── Function ── */
    {uint32_t w[]={OP(33,3),id_fnty,id_void};sb_pn(&b,w,3);}
    {uint32_t w[]={OP(54,5),id_void,id_main,0,id_fnty};sb_pn(&b,w,5);}
    {uint32_t w[]={OP(248,2),id_label};sb_pn(&b,w,2);}

    /* Load InvocationID */
    {uint32_t w[]={OP(61,4),id_int,id_inv,id_invoc};sb_pn(&b,w,4);}

    /* Copy gl_Position */
    {uint32_t w[]={OP(65,6),id_pIv4,id_inptr,id_gl_in,id_inv,id_i0};sb_pn(&b,w,6);}
    {uint32_t w[]={OP(61,4),id_vec4,id_pos,id_inptr};sb_pn(&b,w,4);}
    {uint32_t w[]={OP(65,6),id_pOv4,id_optr,id_gl_out,id_inv,id_i0};sb_pn(&b,w,6);}
    {uint32_t w[]={OP(62,3),id_optr,id_pos};sb_pn(&b,w,3);}

    /* Copy user varyings: inVar[invoc_id] → outVar[invoc_id] */
    for(int u=0;u<n_uvars;u++){
        uint32_t vs=uvars[u].vec_size, tv=id_uvec[vs];
        uint32_t pI=id_pI_uvec[vs], pO=id_pO_uvec[vs];
        {uint32_t w[]={OP(65,5),pI,id_uti[u],id_uvi[u],id_inv};sb_pn(&b,w,5);}
        {uint32_t w[]={OP(61,4),tv,id_uvl[u],id_uti[u]};sb_pn(&b,w,4);}
        {uint32_t w[]={OP(65,5),pO,id_uto[u],id_uvo[u],id_inv};sb_pn(&b,w,5);}
        {uint32_t w[]={OP(62,3),id_uto[u],id_uvl[u]};sb_pn(&b,w,3);}
    }

    /* TessLevels = 1.0 */
    {uint32_t w[]={OP(65,5),id_pOf,id_tl[0],id_tl_i,id_i0};sb_pn(&b,w,5);}
    {uint32_t w[]={OP(62,3),id_tl[0],id_f1};sb_pn(&b,w,3);}
    {uint32_t w[]={OP(65,5),id_pOf,id_tl[1],id_tl_i,id_i1};sb_pn(&b,w,5);}
    {uint32_t w[]={OP(62,3),id_tl[1],id_f1};sb_pn(&b,w,3);}
    {uint32_t w[]={OP(65,5),id_pOf,id_tl[2],id_tl_o,id_i0};sb_pn(&b,w,5);}
    {uint32_t w[]={OP(62,3),id_tl[2],id_f1};sb_pn(&b,w,3);}
    {uint32_t w[]={OP(65,5),id_pOf,id_tl[3],id_tl_o,id_i1};sb_pn(&b,w,5);}
    {uint32_t w[]={OP(62,3),id_tl[3],id_f1};sb_pn(&b,w,3);}
    {uint32_t w[]={OP(65,5),id_pOf,id_tl[4],id_tl_o,id_i2};sb_pn(&b,w,5);}
    {uint32_t w[]={OP(62,3),id_tl[4],id_f1};sb_pn(&b,w,3);}
    {uint32_t w[]={OP(65,5),id_pOf,id_tl[5],id_tl_o,id_i3};sb_pn(&b,w,5);}
    {uint32_t w[]={OP(62,3),id_tl[5],id_f1};sb_pn(&b,w,3);}

    {uint32_t w[]={OP(253,1)};sb_pn(&b,w,1);}
    {uint32_t w[]={OP(56,1)};sb_pn(&b,w,1);}

    b.w[3]=bound; *out=b.w; *out_c=b.n;
    return true;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Base TES builder — triangles/equal/ccw, barycentric interpolation.
 * Interpolates gl_Position AND all user-defined varyings from VS.
 * ═══════════════════════════════════════════════════════════════════════════ */
bool build_base_tes_spv(const uint32_t *vs_spv, size_t vs_wc,
                        uint32_t **out, size_t *out_c)
{
    UserVar uvars[MAX_USER_VARS]; int n_uvars=vs_spv?scan_vs_user_vars(vs_spv,vs_wc,uvars):0;
    STEREO_LOG("build_base_tes_spv: %d user vars",n_uvars);

    bool need_vs[5]={false};
    for(int u=0;u<n_uvars;u++) need_vs[uvars[u].vec_size]=true;

    /* ── ID allocation ─────────────────────────────────────────────────── */
    uint32_t nid=1;
    uint32_t id_void=nid++,id_int=nid++,id_uint=nid++;
    uint32_t id_float=nid++,id_v4=nid++,id_v3=nid++;
    uint32_t id_c32=nid++;
    uint32_t id_PVin=nid++,id_PVinarr=nid++,id_PVout=nid++;
    uint32_t id_pIarr=nid++,id_pOPV=nid++;
    uint32_t id_pIv4=nid++,id_pOv4=nid++,id_pIv3=nid++;
    uint32_t id_glin=nid++,id_glout=nid++,id_tc=nid++;
    uint32_t id_fnty=nid++,id_main=nid++;
    uint32_t id_i0=nid++,id_i1=nid++,id_i2=nid++;

    /* User varying vector types */
    uint32_t id_uvec[5]={0};
    id_uvec[1]=id_float; id_uvec[3]=id_v3; id_uvec[4]=id_v4;
    if(need_vs[2]) id_uvec[2]=nid++;

    /* User varying input array types: array(T,32) */
    uint32_t id_uarr32[5]={0};
    for(int vs=1;vs<=4;vs++) if(need_vs[vs]) id_uarr32[vs]=nid++;

    /* User varying pointer types.
     * Reuse: pIv4 for vs=4 in-elem, pIv3 for vs=3 in-elem, pOv4 for vs=4 out.
     * New: vs=1,2 in-elem; vs=1,2,3 out; all array ptrs. */
    uint32_t id_pI_uvec[5]={0},id_pO_uvec[5]={0};
    uint32_t id_pI_uarr32[5]={0};
    id_pI_uvec[4]=id_pIv4;  /* REUSE ptr Input vec4 */
    id_pI_uvec[3]=id_pIv3;  /* REUSE ptr Input vec3 (used for TessCoord) */
    id_pO_uvec[4]=id_pOv4;  /* REUSE ptr Output vec4 */
    for(int vs=1;vs<=4;vs++){
        if(!need_vs[vs]) continue;
        if(vs!=4&&vs!=3) id_pI_uvec[vs]=nid++;  /* new Input elem ptr for vs=1,2 */
        if(vs!=4)        id_pO_uvec[vs]=nid++;   /* new Output var ptr for vs=1,2,3 */
        id_pI_uarr32[vs]=nid++;
    }

    /* Per-user-var variable IDs */
    uint32_t id_uvi[MAX_USER_VARS]={0},id_uvo[MAX_USER_VARS]={0};
    for(int u=0;u<n_uvars;u++){id_uvi[u]=nid++;id_uvo[u]=nid++;}

    /* Function body */
    uint32_t id_lb=nid++,id_tcv=nid++,id_tcx=nid++,id_tcy=nid++,id_tcz=nid++;
    uint32_t id_p0=nid++,id_in0=nid++,id_p1=nid++,id_in1=nid++,id_p2=nid++,id_in2=nid++;
    uint32_t id_s0=nid++,id_s1=nid++,id_s2=nid++,id_a01=nid++,id_a012=nid++,id_op=nid++;

    /* Per-user-var body: 3 load pairs + 3 scales + 2 adds = 11 SSA values per var */
    uint32_t id_up[MAX_USER_VARS][3]={{0}};
    uint32_t id_uv[MAX_USER_VARS][3]={{0}};
    uint32_t id_us[MAX_USER_VARS][3]={{0}};
    uint32_t id_ua01[MAX_USER_VARS]={0},id_ua012[MAX_USER_VARS]={0};
    for(int u=0;u<n_uvars;u++){
        for(int k=0;k<3;k++){id_up[u][k]=nid++;id_uv[u][k]=nid++;id_us[u][k]=nid++;}
        id_ua01[u]=nid++; id_ua012[u]=nid++;
    }

    uint32_t bound=nid;

    SB b; if(!sb_i(&b,500+n_uvars*70)) return false;

    /* Header */
    sb_p(&b,0x07230203u);sb_p(&b,0x00010000u);sb_p(&b,0x56533344u);sb_p(&b,bound);sb_p(&b,0u);

    /* Capabilities */
    {uint32_t w[]={OP(17,2),1};sb_pn(&b,w,2);}
    {uint32_t w[]={OP(17,2),34};sb_pn(&b,w,2);}

    /* MemoryModel */
    {uint32_t w[]={OP(14,3),0,1};sb_pn(&b,w,3);}

    /* EntryPoint: TES(2) main "main" glin glout tc [user in/out ...] */
    {
        uint32_t ifc=3+(uint32_t)(n_uvars*2);
        sb_p(&b,OP(15,5+ifc)); sb_p(&b,2); sb_p(&b,id_main);
        sb_p(&b,0x6E69616Du); sb_p(&b,0x00000000u);
        sb_p(&b,id_glin);sb_p(&b,id_glout);sb_p(&b,id_tc);
        for(int u=0;u<n_uvars;u++){sb_p(&b,id_uvi[u]);sb_p(&b,id_uvo[u]);}
    }
    /* ExecutionModes: Triangles=22, SpacingEqual=1, VertexOrderCcw=5 */
    {uint32_t w[]={OP(16,3),id_main,22};sb_pn(&b,w,3);}  /* Triangles */
    {uint32_t w[]={OP(16,3),id_main,1};sb_pn(&b,w,3);}   /* SpacingEqual */
    {uint32_t w[]={OP(16,3),id_main,5};sb_pn(&b,w,3);}   /* VertexOrderCcw */

    /* ── Decorations ── */
    {uint32_t w[]={OP(71,3),id_PVin,2};sb_pn(&b,w,3);}
    {uint32_t w[]={OP(72,5),id_PVin,0,11,0};sb_pn(&b,w,5);}
    {uint32_t w[]={OP(71,3),id_PVout,2};sb_pn(&b,w,3);}
    {uint32_t w[]={OP(72,5),id_PVout,0,11,0};sb_pn(&b,w,5);}
    {uint32_t w[]={OP(71,4),id_tc,11,13};sb_pn(&b,w,4);}  /* TessCoord */
    for(int u=0;u<n_uvars;u++){
        {uint32_t w[]={OP(71,4),id_uvi[u],30,uvars[u].location};sb_pn(&b,w,4);}
        {uint32_t w[]={OP(71,4),id_uvo[u],30,uvars[u].location};sb_pn(&b,w,4);}
    }

    /* ── Types ── */
    {uint32_t w[]={OP(19,2),id_void};sb_pn(&b,w,2);}
    {uint32_t w[]={OP(21,4),id_int,32,1};sb_pn(&b,w,4);}
    {uint32_t w[]={OP(21,4),id_uint,32,0};sb_pn(&b,w,4);}
    {uint32_t w[]={OP(22,3),id_float,32};sb_pn(&b,w,3);}
    {uint32_t w[]={OP(23,4),id_v4,id_float,4};sb_pn(&b,w,4);}
    {uint32_t w[]={OP(23,4),id_v3,id_float,3};sb_pn(&b,w,4);}
    if(need_vs[2]&&id_uvec[2]){uint32_t w[]={OP(23,4),id_uvec[2],id_float,2};sb_pn(&b,w,4);}
    {uint32_t w[]={OP(43,4),id_uint,id_c32,32};sb_pn(&b,w,4);}

    /* Block struct/array types */
    {uint32_t w[]={OP(30,3),id_PVin,id_v4};sb_pn(&b,w,3);}
    {uint32_t w[]={OP(28,4),id_PVinarr,id_PVin,id_c32};sb_pn(&b,w,4);}
    {uint32_t w[]={OP(30,3),id_PVout,id_v4};sb_pn(&b,w,3);}

    /* User varying input array types */
    for(int vs=1;vs<=4;vs++) if(need_vs[vs]){
        uint32_t w[]={OP(28,4),id_uarr32[vs],id_uvec[vs],id_c32};sb_pn(&b,w,4);}

    /* Pointer types for block */
    {uint32_t w[]={OP(32,4),id_pIarr,1,id_PVinarr};sb_pn(&b,w,4);}
    {uint32_t w[]={OP(32,4),id_pOPV,3,id_PVout};sb_pn(&b,w,4);}
    {uint32_t w[]={OP(32,4),id_pIv4,1,id_v4};sb_pn(&b,w,4);}
    {uint32_t w[]={OP(32,4),id_pOv4,3,id_v4};sb_pn(&b,w,4);}
    {uint32_t w[]={OP(32,4),id_pIv3,1,id_v3};sb_pn(&b,w,4);}

    /* User varying pointer types (only emit NEW ones) */
    for(int vs=1;vs<=4;vs++){
        if(!need_vs[vs]) continue;
        uint32_t tv=id_uvec[vs];
        /* Input element ptr: new for vs=1,2 */
        if(vs!=4&&vs!=3&&id_pI_uvec[vs]){
            uint32_t w[]={OP(32,4),id_pI_uvec[vs],1,tv};sb_pn(&b,w,4);}
        /* Output ptr: new for vs=1,2,3 */
        if(vs!=4&&id_pO_uvec[vs]){
            uint32_t w[]={OP(32,4),id_pO_uvec[vs],3,tv};sb_pn(&b,w,4);}
        /* Input array ptr: always new */
        {uint32_t w[]={OP(32,4),id_pI_uarr32[vs],1,id_uarr32[vs]};sb_pn(&b,w,4);}
    }

    /* ── Variables ── */
    {uint32_t w[]={OP(59,4),id_pIarr,id_glin,1};sb_pn(&b,w,4);}
    {uint32_t w[]={OP(59,4),id_pOPV,id_glout,3};sb_pn(&b,w,4);}
    {uint32_t w[]={OP(59,4),id_pIv3,id_tc,1};sb_pn(&b,w,4);}
    for(int u=0;u<n_uvars;u++){
        uint32_t vs=uvars[u].vec_size;
        {uint32_t w[]={OP(59,4),id_pI_uarr32[vs],id_uvi[u],1};sb_pn(&b,w,4);}
        /* Output: single element, type = ptr Output T */
        uint32_t pO=(vs==4)?id_pOv4:id_pO_uvec[vs];
        {uint32_t w[]={OP(59,4),pO,id_uvo[u],3};sb_pn(&b,w,4);}
    }

    /* ── Constants ── */
    {uint32_t w[]={OP(43,4),id_int,id_i0,0};sb_pn(&b,w,4);}
    {uint32_t w[]={OP(43,4),id_int,id_i1,1};sb_pn(&b,w,4);}
    {uint32_t w[]={OP(43,4),id_int,id_i2,2};sb_pn(&b,w,4);}

    /* ── Function ── */
    {uint32_t w[]={OP(33,3),id_fnty,id_void};sb_pn(&b,w,3);}
    {uint32_t w[]={OP(54,5),id_void,id_main,0,id_fnty};sb_pn(&b,w,5);}
    {uint32_t w[]={OP(248,2),id_lb};sb_pn(&b,w,2);}

    /* Load TessCoord components */
    {uint32_t w[]={OP(61,4),id_v3,id_tcv,id_tc};sb_pn(&b,w,4);}
    {uint32_t w[]={OP(81,5),id_float,id_tcx,id_tcv,0};sb_pn(&b,w,5);}
    {uint32_t w[]={OP(81,5),id_float,id_tcy,id_tcv,1};sb_pn(&b,w,5);}
    {uint32_t w[]={OP(81,5),id_float,id_tcz,id_tcv,2};sb_pn(&b,w,5);}

    /* Barycentric interpolation of gl_Position */
    {uint32_t w[]={OP(65,6),id_pIv4,id_p0,id_glin,id_i0,id_i0};sb_pn(&b,w,6);}
    {uint32_t w[]={OP(61,4),id_v4,id_in0,id_p0};sb_pn(&b,w,4);}
    {uint32_t w[]={OP(65,6),id_pIv4,id_p1,id_glin,id_i1,id_i0};sb_pn(&b,w,6);}
    {uint32_t w[]={OP(61,4),id_v4,id_in1,id_p1};sb_pn(&b,w,4);}
    {uint32_t w[]={OP(65,6),id_pIv4,id_p2,id_glin,id_i2,id_i0};sb_pn(&b,w,6);}
    {uint32_t w[]={OP(61,4),id_v4,id_in2,id_p2};sb_pn(&b,w,4);}
    {uint32_t w[]={OP(142,5),id_v4,id_s0,id_in0,id_tcx};sb_pn(&b,w,5);}
    {uint32_t w[]={OP(142,5),id_v4,id_s1,id_in1,id_tcy};sb_pn(&b,w,5);}
    {uint32_t w[]={OP(142,5),id_v4,id_s2,id_in2,id_tcz};sb_pn(&b,w,5);}
    {uint32_t w[]={OP(129,5),id_v4,id_a01,id_s0,id_s1};sb_pn(&b,w,5);}
    {uint32_t w[]={OP(129,5),id_v4,id_a012,id_a01,id_s2};sb_pn(&b,w,5);}
    {uint32_t w[]={OP(65,5),id_pOv4,id_op,id_glout,id_i0};sb_pn(&b,w,5);}
    {uint32_t w[]={OP(62,3),id_op,id_a012};sb_pn(&b,w,3);}

    /* Barycentric interpolation of user varyings */
    for(int u=0;u<n_uvars;u++){
        uint32_t vs=uvars[u].vec_size, tv=id_uvec[vs];
        uint32_t pI=id_pI_uvec[vs];
        /* VectorTimesScalar for vec, FMul for scalar */
        uint32_t sop=(vs>1)?142:133;

        {uint32_t w[]={OP(65,5),pI,id_up[u][0],id_uvi[u],id_i0};sb_pn(&b,w,5);}
        {uint32_t w[]={OP(61,4),tv,id_uv[u][0],id_up[u][0]};sb_pn(&b,w,4);}
        {uint32_t w[]={OP(65,5),pI,id_up[u][1],id_uvi[u],id_i1};sb_pn(&b,w,5);}
        {uint32_t w[]={OP(61,4),tv,id_uv[u][1],id_up[u][1]};sb_pn(&b,w,4);}
        {uint32_t w[]={OP(65,5),pI,id_up[u][2],id_uvi[u],id_i2};sb_pn(&b,w,5);}
        {uint32_t w[]={OP(61,4),tv,id_uv[u][2],id_up[u][2]};sb_pn(&b,w,4);}
        {uint32_t w[]={OP(sop,5),tv,id_us[u][0],id_uv[u][0],id_tcx};sb_pn(&b,w,5);}
        {uint32_t w[]={OP(sop,5),tv,id_us[u][1],id_uv[u][1],id_tcy};sb_pn(&b,w,5);}
        {uint32_t w[]={OP(sop,5),tv,id_us[u][2],id_uv[u][2],id_tcz};sb_pn(&b,w,5);}
        {uint32_t w[]={OP(129,5),tv,id_ua01[u],id_us[u][0],id_us[u][1]};sb_pn(&b,w,5);}
        {uint32_t w[]={OP(129,5),tv,id_ua012[u],id_ua01[u],id_us[u][2]};sb_pn(&b,w,5);}
        /* Store to single output variable (no AccessChain needed) */
        {uint32_t w[]={OP(62,3),id_uvo[u],id_ua012[u]};sb_pn(&b,w,3);}
    }

    {uint32_t w[]={OP(253,1)};sb_pn(&b,w,1);}
    {uint32_t w[]={OP(56,1)};sb_pn(&b,w,1);}

    b.w[3]=bound; *out=b.w; *out_c=b.n;
    return true;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Pass-through GS builder
 *
 * Injected between TES and FS for VS-only TRIANGLE_LIST pipelines.
 * On NVIDIA 426.06, gl_ViewIndex in TES is only populated when a GS follows.
 * This GS:
 *   1. Passes gl_Position from TES to output unchanged
 *   2. Explicitly writes gl_Layer = gl_ViewIndex (correct layer selection)
 *   3. Passes all user-defined varyings through per-vertex
 * ═══════════════════════════════════════════════════════════════════════════ */
bool build_passthrough_gs_spv(const uint32_t *vs_spv, size_t vs_wc,
                              uint32_t **out, size_t *out_c)
{
    UserVar uvars[MAX_USER_VARS]; int n_uvars=vs_spv?scan_vs_user_vars(vs_spv,vs_wc,uvars):0;
    STEREO_LOG("build_passthrough_gs_spv: %d user vars",n_uvars);

    bool need_vs[5]={false};
    for(int u=0;u<n_uvars;u++) need_vs[uvars[u].vec_size]=true;

    /* ── ID allocation ─────────────────────────────────────────────────── */
    uint32_t nid=1;
    uint32_t id_void=nid++,id_int=nid++,id_uint=nid++;
    uint32_t id_float=nid++,id_v4=nid++;

    /* gl_ViewIndex input */
    uint32_t id_viewidx_var=nid++;

    /* gl_PerVertex in/out structs */
    uint32_t id_pv_in=nid++,id_pv_out=nid++;
    uint32_t id_c3=nid++,id_c1=nid++;
    uint32_t id_arr_in=nid++; /* array(pv_in, 3) for gl_in */

    /* Pointer types for block */
    uint32_t id_pIarr=nid++;   /* ptr Input array(pv_in,3) */
    uint32_t id_pOPV=nid++;    /* ptr Output pv_out */
    uint32_t id_pIv4=nid++;    /* ptr Input vec4 (for Position access) */
    uint32_t id_pOv4=nid++;    /* ptr Output vec4 */
    uint32_t id_pIint=nid++;   /* ptr Input int (for ViewIndex) */
    uint32_t id_pOint=nid++;   /* ptr Output int (for Layer in pv_out) */

    /* Variables */
    uint32_t id_gl_in=nid++,id_gl_out=nid++,id_viv=nid++;

    /* Constants */
    uint32_t id_i0=nid++,id_i1=nid++,id_i2=nid++;

    /* User varying vector types (new ones only; float/vec4 already declared) */
    uint32_t id_uvec[5]={0};
    id_uvec[1]=id_float; id_uvec[4]=id_v4;
    if(need_vs[2]) id_uvec[2]=nid++;
    if(need_vs[3]) id_uvec[3]=nid++;

    /* User varying array input types: array(T,3) for GS triangles input */
    uint32_t id_uarr3[5]={0};
    for(int vs=1;vs<=4;vs++) if(need_vs[vs]) id_uarr3[vs]=nid++;

    /* User varying ptr types */
    uint32_t id_pI_uvec[5]={0};   /* ptr Input T (AccessChain result into input array) */
    uint32_t id_pO_uvec[5]={0};   /* ptr Output T (single output variable type) */
    uint32_t id_pI_uarr3[5]={0};  /* ptr Input array(T,3) */
    id_pI_uvec[4]=id_pIv4;        /* REUSE ptr Input vec4 */
    id_pO_uvec[4]=id_pOv4;        /* REUSE ptr Output vec4 */
    for(int vs=1;vs<=4;vs++){
        if(!need_vs[vs]) continue;
        if(vs!=4) id_pI_uvec[vs]=nid++;
        if(vs!=4) id_pO_uvec[vs]=nid++;
        id_pI_uarr3[vs]=nid++;
    }

    /* Per-user-var variable IDs */
    uint32_t id_uvi[MAX_USER_VARS]={0},id_uvo[MAX_USER_VARS]={0};
    for(int u=0;u<n_uvars;u++){id_uvi[u]=nid++;id_uvo[u]=nid++;}

    /* Function body */
    uint32_t id_fnty=nid++,id_main=nid++,id_label=nid++;
    /* Per vertex (×3): pos_ptr, pos_val, out_pos_ptr, view_val, layer_ptr, + user vars×2 */
    /* 3 vertices × (5 + n_uvars×3) SSA values */
    uint32_t id_body[3][1 + 16 * (1 + MAX_USER_VARS)]; /* generous buffer */
    memset(id_body,0,sizeof(id_body));
    for(int v=0;v<3;v++){
        for(int k=0;k<(5 + n_uvars*3);k++) id_body[v][k]=nid++;
    }
    uint32_t id_emit_dummy[3]; /* not actual SSA, just placeholders */
    (void)id_emit_dummy;

    uint32_t bound=nid;

    SB b; if(!sb_i(&b,400+n_uvars*60)) return false;

    /* Header */
    sb_p(&b,0x07230203u);sb_p(&b,0x00010000u);sb_p(&b,0x56533344u);sb_p(&b,bound);sb_p(&b,0u);

    /* Capabilities: Shader + Geometry + MultiView (for gl_ViewIndex in GS) */
    {uint32_t w[]={OP(17,2),1};sb_pn(&b,w,2);}   /* Shader */
    {uint32_t w[]={OP(17,2),5296};sb_pn(&b,w,2);}/* MultiView (SpvCapabilityMultiView) */
    {uint32_t w[]={OP(17,2),2};sb_pn(&b,w,2);}   /* Geometry */

    /* MemoryModel */
    {uint32_t w[]={OP(14,3),0,1};sb_pn(&b,w,3);}

    /* EntryPoint: Geometry=3, main, "main", all interface vars */
    {
        uint32_t ifc = 3 + (uint32_t)(n_uvars*2); /* gl_in, gl_out, viv (gl_Layer is member of gl_out, not a separate var) */
        sb_p(&b,OP(15,5+ifc)); sb_p(&b,3); sb_p(&b,id_main);
        sb_p(&b,0x6E69616Du); sb_p(&b,0x00000000u);
        sb_p(&b,id_gl_in); sb_p(&b,id_gl_out); sb_p(&b,id_viv);
        /* ViewIndex variable */
        for(int u=0;u<n_uvars;u++){sb_p(&b,id_uvi[u]);sb_p(&b,id_uvo[u]);}
    }
    /* ExecutionModes */
    /* GS ExecutionModes: Triangles=22 input, OutputTriangleStrip=29, OutputVertices=26 */
    {uint32_t w[]={OP(16,3),id_main,22};sb_pn(&b,w,3);}   /* Triangles (input) */
    {uint32_t w[]={OP(16,3),id_main,29};sb_pn(&b,w,3);}   /* OutputTriangleStrip */
    {uint32_t w[]={OP(16,4),id_main,26,3};sb_pn(&b,w,4);} /* OutputVertices 3 */

    /* ── Decorations ── */
    /* gl_PerVertex_in Block + Position */
    {uint32_t w[]={OP(71,3),id_pv_in,2};sb_pn(&b,w,3);}
    {uint32_t w[]={OP(72,5),id_pv_in,0,11,0};sb_pn(&b,w,5);}
    /* gl_PerVertex_out Block + Position(0) + Layer(1) */
    {uint32_t w[]={OP(71,3),id_pv_out,2};sb_pn(&b,w,3);}
    {uint32_t w[]={OP(72,5),id_pv_out,0,11,0};sb_pn(&b,w,5);}  /* Position */
    {uint32_t w[]={OP(72,5),id_pv_out,1,11,9};sb_pn(&b,w,5);}  /* Layer=9 */
    /* ViewIndex */
    {uint32_t w[]={OP(71,4),id_viv,11,4440};sb_pn(&b,w,4);}
    /* User varying locations */
    for(int u=0;u<n_uvars;u++){
        {uint32_t w[]={OP(71,4),id_uvi[u],30,uvars[u].location};sb_pn(&b,w,4);}
        {uint32_t w[]={OP(71,4),id_uvo[u],30,uvars[u].location};sb_pn(&b,w,4);}
    }

    /* ── Types ── */
    {uint32_t w[]={OP(19,2),id_void};sb_pn(&b,w,2);}
    {uint32_t w[]={OP(21,4),id_int,32,1};sb_pn(&b,w,4);}
    {uint32_t w[]={OP(21,4),id_uint,32,0};sb_pn(&b,w,4);}
    {uint32_t w[]={OP(22,3),id_float,32};sb_pn(&b,w,3);}
    {uint32_t w[]={OP(23,4),id_v4,id_float,4};sb_pn(&b,w,4);}
    if(need_vs[2]&&id_uvec[2]){uint32_t w[]={OP(23,4),id_uvec[2],id_float,2};sb_pn(&b,w,4);}
    if(need_vs[3]&&id_uvec[3]){uint32_t w[]={OP(23,4),id_uvec[3],id_float,3};sb_pn(&b,w,4);}
    {uint32_t w[]={OP(43,4),id_uint,id_c3,3};sb_pn(&b,w,4);}
    {uint32_t w[]={OP(43,4),id_uint,id_c1,1};sb_pn(&b,w,4);}

    /* Block structs */
    {uint32_t w[]={OP(30,3),id_pv_in,id_v4};sb_pn(&b,w,3);}               /* pv_in{vec4} */
    {uint32_t w[]={OP(30,4),id_pv_out,id_v4,id_int};sb_pn(&b,w,4);}       /* pv_out{vec4,int} */
    {uint32_t w[]={OP(28,4),id_arr_in,id_pv_in,id_c3};sb_pn(&b,w,4);}     /* array(pv_in,3) */

    /* User varying array types for input: array(T,3) */
    for(int vs=1;vs<=4;vs++) if(need_vs[vs]){
        uint32_t w[]={OP(28,4),id_uarr3[vs],id_uvec[vs],id_c3};sb_pn(&b,w,4);}

    /* Pointer types */
    {uint32_t w[]={OP(32,4),id_pIarr,1,id_arr_in};sb_pn(&b,w,4);}
    {uint32_t w[]={OP(32,4),id_pOPV,3,id_pv_out};sb_pn(&b,w,4);}
    {uint32_t w[]={OP(32,4),id_pIv4,1,id_v4};sb_pn(&b,w,4);}
    {uint32_t w[]={OP(32,4),id_pOv4,3,id_v4};sb_pn(&b,w,4);}
    {uint32_t w[]={OP(32,4),id_pIint,1,id_int};sb_pn(&b,w,4);}
    {uint32_t w[]={OP(32,4),id_pOint,3,id_int};sb_pn(&b,w,4);}
    for(int vs=1;vs<=4;vs++){
        if(!need_vs[vs]) continue;
        uint32_t tv=id_uvec[vs];
        if(vs!=4) {uint32_t w[]={OP(32,4),id_pI_uvec[vs],1,tv};sb_pn(&b,w,4);}
        if(vs!=4) {uint32_t w[]={OP(32,4),id_pO_uvec[vs],3,tv};sb_pn(&b,w,4);}
        {uint32_t w[]={OP(32,4),id_pI_uarr3[vs],1,id_uarr3[vs]};sb_pn(&b,w,4);}
    }

    /* ── Variables ── */
    {uint32_t w[]={OP(59,4),id_pIarr,id_gl_in,1};sb_pn(&b,w,4);}
    {uint32_t w[]={OP(59,4),id_pOPV,id_gl_out,3};sb_pn(&b,w,4);}
    {uint32_t w[]={OP(59,4),id_pIint,id_viv,1};sb_pn(&b,w,4);}  /* ViewIndex */
    for(int u=0;u<n_uvars;u++){
        uint32_t vs=uvars[u].vec_size, pO=(vs==4)?id_pOv4:id_pO_uvec[vs];
        {uint32_t w[]={OP(59,4),id_pI_uarr3[vs],id_uvi[u],1};sb_pn(&b,w,4);}
        {uint32_t w[]={OP(59,4),pO,id_uvo[u],3};sb_pn(&b,w,4);}
    }

    /* ── Constants ── */
    {uint32_t w[]={OP(43,4),id_int,id_i0,0};sb_pn(&b,w,4);}
    {uint32_t w[]={OP(43,4),id_int,id_i1,1};sb_pn(&b,w,4);}
    {uint32_t w[]={OP(43,4),id_int,id_i2,2};sb_pn(&b,w,4);}

    /* ── Function ── */
    {uint32_t w[]={OP(33,3),id_fnty,id_void};sb_pn(&b,w,3);}
    {uint32_t w[]={OP(54,5),id_void,id_main,0,id_fnty};sb_pn(&b,w,5);}
    {uint32_t w[]={OP(248,2),id_label};sb_pn(&b,w,2);}

    /* Load gl_ViewIndex once */
    uint32_t id_view = id_body[0][0]; /* reuse first slot for the view value */
    {uint32_t w[]={OP(61,4),id_int,id_view,id_viv};sb_pn(&b,w,4);}

    /* Unrolled loop: emit vertex 0, 1, 2 */
    uint32_t vert_idx[3] = {id_i0, id_i1, id_i2};
    for(int v=0; v<3; v++){
        uint32_t vi = vert_idx[v];
        int slot = 1; /* slot 0 used for id_view */

        /* Copy gl_Position from gl_in[v].member0 */
        uint32_t pos_ptr = id_body[v][slot++];
        uint32_t pos_val = id_body[v][slot++];
        uint32_t opos_ptr= id_body[v][slot++];
        {uint32_t w[]={OP(65,6),id_pIv4,pos_ptr,id_gl_in,vi,id_i0};sb_pn(&b,w,6);}
        {uint32_t w[]={OP(61,4),id_v4,pos_val,pos_ptr};sb_pn(&b,w,4);}
        {uint32_t w[]={OP(65,5),id_pOv4,opos_ptr,id_gl_out,id_i0};sb_pn(&b,w,5);}
        {uint32_t w[]={OP(62,3),opos_ptr,pos_val};sb_pn(&b,w,3);}

        /* Write gl_Layer = gl_ViewIndex */
        uint32_t lptr = id_body[v][slot++];
        {uint32_t w[]={OP(65,5),id_pOint,lptr,id_gl_out,id_i1};sb_pn(&b,w,5);}
        {uint32_t w[]={OP(62,3),lptr,id_view};sb_pn(&b,w,3);}

        /* Copy user varyings */
        for(int u=0; u<n_uvars; u++){
            uint32_t vs=uvars[u].vec_size, tv=id_uvec[vs], pI=id_pI_uvec[vs];
            uint32_t uptr = id_body[v][slot++];
            uint32_t uval = id_body[v][slot++];
            uint32_t pO = (vs==4)?id_pOv4:id_pO_uvec[vs];
            {uint32_t w[]={OP(65,5),pI,uptr,id_uvi[u],vi};sb_pn(&b,w,5);}
            {uint32_t w[]={OP(61,4),tv,uval,uptr};sb_pn(&b,w,4);}
            {uint32_t w[]={OP(62,3),id_uvo[u],uval};sb_pn(&b,w,3);}
        }

        /* EmitVertex */
        {uint32_t w[]={OP(218,1)};sb_pn(&b,w,1);}
    }
    /* EndPrimitive */
    {uint32_t w[]={OP(219,1)};sb_pn(&b,w,1);}

    {uint32_t w[]={OP(253,1)};sb_pn(&b,w,1);}
    {uint32_t w[]={OP(56,1)};sb_pn(&b,w,1);}

    b.w[3]=bound; *out=b.w; *out_c=b.n;
    return true;
}
