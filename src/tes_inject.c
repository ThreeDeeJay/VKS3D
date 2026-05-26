/*
 * tes_inject.c — Build pass-through TCS and stereo TES SPIR-V at runtime
 *
 * The TCS gl_in struct MUST match the VS output block exactly.
 * We scan the cached VS SPIR-V to find its gl_PerVertex layout and
 * replicate it in the TCS input declaration.
 *
 * BuiltIn  = 11, Block = 2, Patch = 15
 * TessLevelOuter = 11, TessLevelInner = 12, TessCoord = 13, InvocationId = 8
 * Position = 0,  PointSize = 1, ClipDistance = 3, CullDistance = 4
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "stereo_icd.h"

typedef struct { uint32_t *w; size_t n, cap; } SB;
static bool sb_i(SB *b, size_t c) { b->w=malloc(c*4); b->n=0; b->cap=c; return !!b->w; }
static bool sb_p(SB *b, uint32_t v) {
    if (b->n>=b->cap){uint32_t *p=realloc(b->w,b->cap*8);if(!p)return false;b->w=p;b->cap*=2;}
    b->w[b->n++]=v; return true; }
static bool sb_pn(SB *b, const uint32_t *v, size_t c) {
    for(size_t i=0;i<c;i++) if(!sb_p(b,v[i])) return false; return true; }
static uint32_t OP(uint32_t op, uint32_t wc) { return (wc<<16)|op; }

/* ── VS output block scanner ──────────────────────────────────────────────── */

#define MAX_MBRS 8

/* Member kinds as they appear in gl_PerVertex */
typedef enum { MBR_VEC4=0, MBR_FLOAT=1, MBR_FARR=2 } MbrKind;
typedef struct { MbrKind kind; uint32_t arr_n; uint32_t builtin; } Mbr;

/* Scan VS SPIR-V for the Output Block struct and its member types.
 * Returns the number of members found (at least 1 for Position). */
static int scan_vs_block(const uint32_t *spv, size_t wc, Mbr out[MAX_MBRS])
{
    /* Phase 1: find the Block-decorated Output struct (member 0 = BuiltIn Position) */
    uint32_t block_id = 0;
    for (size_t i=5; i<wc; ) {
        uint32_t op=spv[i]&0xffff, n=spv[i]>>16;
        if (!n||i+n>wc) break;
        /* OpMemberDecorate struct member BuiltIn(11) Position(0) */
        if (op==72 && n>=5 && spv[i+2]==0 && spv[i+3]==11 && spv[i+4]==0)
            block_id = spv[i+1];
        i += n;
    }
    if (!block_id) return 0;

    /* Phase 2: find OpTypeStruct result=block_id → get member type IDs */
    uint32_t mbr_types[MAX_MBRS] = {0};
    int n_mbrs = 0;
    for (size_t i=5; i<wc; ) {
        uint32_t op=spv[i]&0xffff, n=spv[i]>>16;
        if (!n||i+n>wc) break;
        if (op==30/*OpTypeStruct*/ && n>=2 && spv[i+1]==block_id) {
            n_mbrs = (int)(n-2);
            if (n_mbrs > MAX_MBRS) n_mbrs = MAX_MBRS;
            for (int m=0;m<n_mbrs;m++) mbr_types[m]=spv[i+2+m];
        }
        i += n;
    }
    if (!n_mbrs) return 0;

    /* Phase 3: for each member type ID, determine kind */
    /* First find float type and build lookup */
    uint32_t float_id = 0;
    for (size_t i=5; i<wc; ) {
        uint32_t op=spv[i]&0xffff, n=spv[i]>>16;
        if (!n||i+n>wc) break;
        if (op==22/*OpTypeFloat*/ && n>=3 && spv[i+2]==32) float_id=spv[i+1];
        i += n;
    }

    for (int m=0; m<n_mbrs; m++) {
        uint32_t tid = mbr_types[m];
        out[m].kind = MBR_FLOAT;
        out[m].arr_n = 0;
        out[m].builtin = 0; /* filled in phase 4 */

        for (size_t i=5; i<wc; ) {
            uint32_t op=spv[i]&0xffff, n=spv[i]>>16;
            if (!n||i+n>wc) break;
            if (spv[i+1]==tid) {
                if (op==22) { /* OpTypeFloat */
                    out[m].kind = MBR_FLOAT;
                } else if (op==23 && n>=4) { /* OpTypeVector */
                    /* vec4 if elem=float and count=4 */
                    out[m].kind = (spv[i+2]==float_id && spv[i+3]==4) ? MBR_VEC4 : MBR_FLOAT;
                } else if (op==28 && n>=4) { /* OpTypeArray */
                    out[m].kind = MBR_FARR;
                    /* find array length constant */
                    uint32_t len_id = spv[i+3];
                    for (size_t j=5; j<wc; ) {
                        uint32_t op2=spv[j]&0xffff, n2=spv[j]>>16;
                        if (!n2||j+n2>wc) break;
                        if (op2==43/*OpConstant*/ && n2>=4 && spv[j+2]==len_id) {
                            out[m].arr_n = spv[j+3];
                        }
                        j += n2;
                    }
                    if (!out[m].arr_n) out[m].arr_n = 1; /* safe fallback */
                }
                break;
            }
            i += n;
        }
    }

    /* Phase 4: fill builtin values from MemberDecorate */
    for (size_t i=5; i<wc; ) {
        uint32_t op=spv[i]&0xffff, n=spv[i]>>16;
        if (!n||i+n>wc) break;
        if (op==72/*OpMemberDecorate*/ && n>=5 &&
            spv[i+1]==block_id && spv[i+3]==11/*BuiltIn*/) {
            uint32_t mem = spv[i+2];
            if (mem < (uint32_t)n_mbrs)
                out[mem].builtin = spv[i+4];
        }
        i += n;
    }

    /* Make sure member 0 is always MBR_VEC4 (Position) */
    if (n_mbrs >= 1) out[0].kind = MBR_VEC4;

    return n_mbrs;
}

/* ── TCS SPIR-V builder ────────────────────────────────────────────────────── *
 *
 * Generates a pass-through TCS that matches the VS output block exactly.
 * gl_in  = matches VS output (all members, exact types)
 * gl_out = only Position (matches our base TES gl_in)
 * Body: copies Position gl_in[invoc_id] → gl_out[invoc_id], sets tess levels=1.
 *
 * Using dynamic ID allocation to handle variable member counts.
 * ─────────────────────────────────────────────────────────────────────────── */
bool build_tcs_spv(const uint32_t *vs_spv, size_t vs_wc,
                   uint32_t **out, size_t *out_c)
{
    /* Scan VS for output block */
    Mbr mbrs[MAX_MBRS];
    int n_mbrs = vs_spv ? scan_vs_block(vs_spv, vs_wc, mbrs) : 0;
    if (n_mbrs < 1) {
        /* Fallback: 1-member block (just Position) */
        n_mbrs = 1;
        mbrs[0].kind = MBR_VEC4; mbrs[0].arr_n = 0; mbrs[0].builtin = 0;
    }
    STEREO_LOG("build_tcs_spv: VS output block has %d members", n_mbrs);

    SB b; if (!sb_i(&b, 400 + n_mbrs*30)) return false;

    /* ── Assign IDs ──────────────────────────────────────────────────────── */
    uint32_t nid = 1;
    uint32_t id_void  = nid++;  /* 1 */
    uint32_t id_int   = nid++;  /* 2 */
    uint32_t id_uint  = nid++;  /* 3 */
    uint32_t id_float = nid++;  /* 4 */
    uint32_t id_vec4  = nid++;  /* 5 */

    /* Array-size constants for struct member arrays */
    uint32_t id_farr_consts[MAX_MBRS]; /* uint constant for each FARR member */
    uint32_t id_farr_types [MAX_MBRS]; /* OpTypeArray for each FARR member */
    for (int m=0; m<n_mbrs; m++) {
        id_farr_consts[m] = (mbrs[m].kind==MBR_FARR) ? nid++ : 0;
        id_farr_types [m] = (mbrs[m].kind==MBR_FARR) ? nid++ : 0;
    }

    /* Array size constants for gl_in/gl_out arrays and tess levels */
    uint32_t id_c32 = nid++;   /* uint const = 32 (gl_in max) */
    uint32_t id_c3  = nid++;   /* uint const = 3  (gl_out patch size) */
    uint32_t id_c2  = nid++;   /* uint const = 2  (tl_inner size) */
    uint32_t id_c4  = nid++;   /* uint const = 4  (tl_outer size) */

    /* Struct types */
    uint32_t id_pv_in  = nid++; /* PerVertex_in  {all VS members} */
    uint32_t id_pv_out = nid++; /* PerVertex_out {vec4 only} */

    /* Array types */
    uint32_t id_arr_in   = nid++;  /* PerV_in[32] */
    uint32_t id_arr_out  = nid++;  /* PerV_out[3] */
    uint32_t id_float_a2 = nid++;  /* float[2] — tl_inner */
    uint32_t id_float_a4 = nid++;  /* float[4] — tl_outer */

    /* Pointer types */
    uint32_t id_ptr_in_arr  = nid++;  /* ptr Input PerV_in[32] */
    uint32_t id_ptr_out_arr = nid++;  /* ptr Output PerV_out[3] */
    uint32_t id_ptr_in_v4   = nid++;  /* ptr Input  vec4 */
    uint32_t id_ptr_out_v4  = nid++;  /* ptr Output vec4 */
    uint32_t id_ptr_in_int  = nid++;  /* ptr Input  int */
    uint32_t id_ptr_out_f2  = nid++;  /* ptr Output float[2] */
    uint32_t id_ptr_out_f4  = nid++;  /* ptr Output float[4] */
    uint32_t id_ptr_out_f   = nid++;  /* ptr Output float */

    /* Variables */
    uint32_t id_gl_in    = nid++;
    uint32_t id_gl_out   = nid++;
    uint32_t id_tl_inner = nid++;
    uint32_t id_tl_outer = nid++;
    uint32_t id_invoc_id = nid++;

    /* Integer/float constants */
    uint32_t id_i0    = nid++;
    uint32_t id_i1    = nid++;
    uint32_t id_i2    = nid++;
    uint32_t id_i3    = nid++;
    uint32_t id_f1    = nid++;

    /* Function */
    uint32_t id_fn_ty = nid++;
    uint32_t id_main  = nid++;
    uint32_t id_label = nid++;

    /* Body SSA: invoc_id_val, in_ptr, pos, out_ptr, tl_ptrs × 6 */
    uint32_t id_inv   = nid++;
    uint32_t id_inptr = nid++;
    uint32_t id_pos   = nid++;
    uint32_t id_optr  = nid++;
    uint32_t id_tl[6];
    for (int k=0; k<6; k++) id_tl[k] = nid++;

    uint32_t bound = nid;

    /* ── Header ─────────────────────────────────────────────────────────── */
    sb_p(&b, 0x07230203u);
    sb_p(&b, 0x00010000u);
    sb_p(&b, 0x56533344u);
    sb_p(&b, bound);
    sb_p(&b, 0u);

    /* ── Capabilities ────────────────────────────────────────────────────── */
    { uint32_t w[]={OP(17,2),1}; sb_pn(&b,w,2); }  /* Shader */
    { uint32_t w[]={OP(17,2),34}; sb_pn(&b,w,2); } /* Tessellation */

    /* ── MemoryModel ─────────────────────────────────────────────────────── */
    { uint32_t w[]={OP(14,3),0,1}; sb_pn(&b,w,3); }

    /* ── EntryPoint ──────────────────────────────────────────────────────── *
     * TessellationControl(1), %main, "main\0", gl_in, gl_out,
     * tl_inner, tl_outer, invoc_id
     */
    { uint32_t w[]={OP(15,10),1,id_main,0x6E69616Du,0x00000000u,
                    id_gl_in,id_gl_out,id_tl_inner,id_tl_outer,id_invoc_id};
      sb_pn(&b,w,10); }

    /* ── ExecutionMode: OutputVertices 3 (SpvExecutionModeOutputVertices=26) */
    { uint32_t w[]={OP(16,4),id_main,26,3}; sb_pn(&b,w,4); }

    /* ── Decorations ─────────────────────────────────────────────────────── */
    /* PerVertex_in: Block */
    { uint32_t w[]={OP(71,3),id_pv_in,2}; sb_pn(&b,w,3); }
    /* PerVertex_in: member decorations (matching VS output) */
    for (int m=0; m<n_mbrs; m++) {
        /* BuiltIn decoration */
        { uint32_t w[]={OP(72,5),id_pv_in,(uint32_t)m,11,mbrs[m].builtin};
          sb_pn(&b,w,5); }
    }
    /* PerVertex_out: Block + member 0 = BuiltIn Position */
    { uint32_t w[]={OP(71,3),id_pv_out,2}; sb_pn(&b,w,3); }
    { uint32_t w[]={OP(72,5),id_pv_out,0,11,0}; sb_pn(&b,w,5); }
    /* tl_inner: BuiltIn TessLevelInner(12) + Patch(15) */
    { uint32_t w[]={OP(71,4),id_tl_inner,11,12}; sb_pn(&b,w,4); }
    { uint32_t w[]={OP(71,3),id_tl_inner,15}; sb_pn(&b,w,3); }
    /* tl_outer: BuiltIn TessLevelOuter(11) + Patch(15) */
    { uint32_t w[]={OP(71,4),id_tl_outer,11,11}; sb_pn(&b,w,4); }
    { uint32_t w[]={OP(71,3),id_tl_outer,15}; sb_pn(&b,w,3); }
    /* invoc_id: BuiltIn InvocationId(8) */
    { uint32_t w[]={OP(71,4),id_invoc_id,11,8}; sb_pn(&b,w,4); }

    /* ── Types ───────────────────────────────────────────────────────────── */
    { uint32_t w[]={OP(19,2),id_void}; sb_pn(&b,w,2); }
    { uint32_t w[]={OP(21,4),id_int, 32,1}; sb_pn(&b,w,4); }
    { uint32_t w[]={OP(21,4),id_uint,32,0}; sb_pn(&b,w,4); }
    { uint32_t w[]={OP(22,3),id_float,32}; sb_pn(&b,w,3); }
    { uint32_t w[]={OP(23,4),id_vec4,id_float,4}; sb_pn(&b,w,4); }

    /* Per-member FARR types (uint constant + OpTypeArray) */
    for (int m=0; m<n_mbrs; m++) {
        if (mbrs[m].kind != MBR_FARR) continue;
        { uint32_t w[]={OP(43,4),id_uint,id_farr_consts[m],mbrs[m].arr_n};
          sb_pn(&b,w,4); }
        { uint32_t w[]={OP(28,4),id_farr_types[m],id_float,id_farr_consts[m]};
          sb_pn(&b,w,4); }
    }

    /* Array size constants */
    { uint32_t w[]={OP(43,4),id_uint,id_c32,32}; sb_pn(&b,w,4); }
    { uint32_t w[]={OP(43,4),id_uint,id_c3, 3};  sb_pn(&b,w,4); }
    { uint32_t w[]={OP(43,4),id_uint,id_c2, 2};  sb_pn(&b,w,4); }
    { uint32_t w[]={OP(43,4),id_uint,id_c4, 4};  sb_pn(&b,w,4); }

    /* PerVertex_in struct: one member type per VS member */
    {
        uint32_t hdr = OP(30, 2 + (uint32_t)n_mbrs);
        sb_p(&b, hdr);
        sb_p(&b, id_pv_in);
        for (int m=0; m<n_mbrs; m++) {
            uint32_t mt = (mbrs[m].kind==MBR_VEC4)  ? id_vec4  :
                          (mbrs[m].kind==MBR_FLOAT)  ? id_float :
                          id_farr_types[m];
            sb_p(&b, mt);
        }
    }

    /* PerVertex_out struct: just vec4 (Position only, matches TES gl_in) */
    { uint32_t w[]={OP(30,3),id_pv_out,id_vec4}; sb_pn(&b,w,3); }

    /* Array types */
    { uint32_t w[]={OP(28,4),id_arr_in,  id_pv_in, id_c32}; sb_pn(&b,w,4); }
    { uint32_t w[]={OP(28,4),id_arr_out, id_pv_out,id_c3};  sb_pn(&b,w,4); }
    { uint32_t w[]={OP(28,4),id_float_a2,id_float,  id_c2}; sb_pn(&b,w,4); }
    { uint32_t w[]={OP(28,4),id_float_a4,id_float,  id_c4}; sb_pn(&b,w,4); }

    /* Pointer types */
    { uint32_t w[]={OP(32,4),id_ptr_in_arr, 1,id_arr_in};   sb_pn(&b,w,4); }
    { uint32_t w[]={OP(32,4),id_ptr_out_arr,3,id_arr_out};  sb_pn(&b,w,4); }
    { uint32_t w[]={OP(32,4),id_ptr_in_v4,  1,id_vec4};     sb_pn(&b,w,4); }
    { uint32_t w[]={OP(32,4),id_ptr_out_v4, 3,id_vec4};     sb_pn(&b,w,4); }
    { uint32_t w[]={OP(32,4),id_ptr_in_int, 1,id_int};      sb_pn(&b,w,4); }
    { uint32_t w[]={OP(32,4),id_ptr_out_f2, 3,id_float_a2}; sb_pn(&b,w,4); }
    { uint32_t w[]={OP(32,4),id_ptr_out_f4, 3,id_float_a4}; sb_pn(&b,w,4); }
    { uint32_t w[]={OP(32,4),id_ptr_out_f,  3,id_float};    sb_pn(&b,w,4); }

    /* Variables */
    { uint32_t w[]={OP(59,4),id_ptr_in_arr, id_gl_in,   1}; sb_pn(&b,w,4); }
    { uint32_t w[]={OP(59,4),id_ptr_out_arr,id_gl_out,  3}; sb_pn(&b,w,4); }
    { uint32_t w[]={OP(59,4),id_ptr_out_f2, id_tl_inner,3}; sb_pn(&b,w,4); }
    { uint32_t w[]={OP(59,4),id_ptr_out_f4, id_tl_outer,3}; sb_pn(&b,w,4); }
    { uint32_t w[]={OP(59,4),id_ptr_in_int, id_invoc_id,1}; sb_pn(&b,w,4); }

    /* Integer / float constants */
    { uint32_t w[]={OP(43,4),id_int,  id_i0,0}; sb_pn(&b,w,4); }
    { uint32_t w[]={OP(43,4),id_int,  id_i1,1}; sb_pn(&b,w,4); }
    { uint32_t w[]={OP(43,4),id_int,  id_i2,2}; sb_pn(&b,w,4); }
    { uint32_t w[]={OP(43,4),id_int,  id_i3,3}; sb_pn(&b,w,4); }
    { uint32_t w[]={OP(43,4),id_float,id_f1, 0x3F800000u}; sb_pn(&b,w,4); }

    /* Function type */
    { uint32_t w[]={OP(33,3),id_fn_ty,id_void}; sb_pn(&b,w,3); }

    /* ── Function body ───────────────────────────────────────────────────── */
    { uint32_t w[]={OP(54,5),id_void,id_main,0,id_fn_ty}; sb_pn(&b,w,5); }
    { uint32_t w[]={OP(248,2),id_label}; sb_pn(&b,w,2); }

    /* Load InvocationID */
    { uint32_t w[]={OP(61,4),id_int,id_inv,id_invoc_id}; sb_pn(&b,w,4); }
    /* AccessChain gl_in[invoc_id].Position (member 0) */
    { uint32_t w[]={OP(65,6),id_ptr_in_v4,id_inptr,id_gl_in,id_inv,id_i0};
      sb_pn(&b,w,6); }
    /* Load Position */
    { uint32_t w[]={OP(61,4),id_vec4,id_pos,id_inptr}; sb_pn(&b,w,4); }
    /* AccessChain gl_out[invoc_id].Position (member 0) */
    { uint32_t w[]={OP(65,6),id_ptr_out_v4,id_optr,id_gl_out,id_inv,id_i0};
      sb_pn(&b,w,6); }
    /* Store Position */
    { uint32_t w[]={OP(62,3),id_optr,id_pos}; sb_pn(&b,w,3); }

    /* TessLevelInner[0] = TessLevelInner[1] = 1.0 */
    { uint32_t w[]={OP(65,5),id_ptr_out_f,id_tl[0],id_tl_inner,id_i0}; sb_pn(&b,w,5); }
    { uint32_t w[]={OP(62,3),id_tl[0],id_f1}; sb_pn(&b,w,3); }
    { uint32_t w[]={OP(65,5),id_ptr_out_f,id_tl[1],id_tl_inner,id_i1}; sb_pn(&b,w,5); }
    { uint32_t w[]={OP(62,3),id_tl[1],id_f1}; sb_pn(&b,w,3); }
    /* TessLevelOuter[0..3] = 1.0 */
    { uint32_t w[]={OP(65,5),id_ptr_out_f,id_tl[2],id_tl_outer,id_i0}; sb_pn(&b,w,5); }
    { uint32_t w[]={OP(62,3),id_tl[2],id_f1}; sb_pn(&b,w,3); }
    { uint32_t w[]={OP(65,5),id_ptr_out_f,id_tl[3],id_tl_outer,id_i1}; sb_pn(&b,w,5); }
    { uint32_t w[]={OP(62,3),id_tl[3],id_f1}; sb_pn(&b,w,3); }
    { uint32_t w[]={OP(65,5),id_ptr_out_f,id_tl[4],id_tl_outer,id_i2}; sb_pn(&b,w,5); }
    { uint32_t w[]={OP(62,3),id_tl[4],id_f1}; sb_pn(&b,w,3); }
    { uint32_t w[]={OP(65,5),id_ptr_out_f,id_tl[5],id_tl_outer,id_i3}; sb_pn(&b,w,5); }
    { uint32_t w[]={OP(62,3),id_tl[5],id_f1}; sb_pn(&b,w,3); }

    { uint32_t w[]={OP(253,1)}; sb_pn(&b,w,1); } /* OpReturn */
    { uint32_t w[]={OP(56,1)};  sb_pn(&b,w,1); } /* OpFunctionEnd */

    b.w[3] = bound; /* restore correct bound */
    *out   = b.w;
    *out_c = b.n;
    return true;
}

/* ── Base TES SPIR-V ──────────────────────────────────────────────────────── */
bool build_base_tes_spv(uint32_t **out, size_t *out_c)
{
    SB b; if (!sb_i(&b, 260)) return false;
    /* IDs: 1=void 2=int 3=uint 4=float 5=v4 6=v3
     * 7=c32 8=PVin{v4} 9=PVin[32] 10=PVout{v4}
     * 11=ptr_In_arr 12=ptr_Out_PV 13=ptr_In_v4 14=ptr_Out_v4 15=ptr_In_v3
     * 16=gl_in 17=gl_out 18=tc_var
     * 19=fn_ty 20=main
     * 21=i0 22=i1 23=i2
     * body: 24..40 */
    uint32_t id_void=1,id_int=2,id_uint=3,id_float=4,id_v4=5,id_v3=6;
    uint32_t id_c32=7,id_PVin=8,id_PVinarr=9,id_PVout=10;
    uint32_t id_pIarr=11,id_pOPV=12,id_pIv4=13,id_pOv4=14,id_pIv3=15;
    uint32_t id_glin=16,id_glout=17,id_tc=18;
    uint32_t id_fnty=19,id_main=20;
    uint32_t id_i0=21,id_i1=22,id_i2=23;
    /* body */
    uint32_t id_lb=24,id_tcv=25,id_tcx=26,id_tcy=27,id_tcz=28;
    uint32_t id_p0=29,id_in0=30,id_p1=31,id_in1=32,id_p2=33,id_in2=34;
    uint32_t id_s0=35,id_s1=36,id_s2=37,id_a01=38,id_a012=39;
    uint32_t id_op=40;

    sb_p(&b,0x07230203u); sb_p(&b,0x00010000u);
    sb_p(&b,0x56533344u); sb_p(&b,41u); sb_p(&b,0u);

    { uint32_t w[]={OP(17,2),1}; sb_pn(&b,w,2); }
    { uint32_t w[]={OP(17,2),34}; sb_pn(&b,w,2); }
    { uint32_t w[]={OP(14,3),0,1}; sb_pn(&b,w,3); }

    /* EntryPoint TessEval(2) main "main" gl_in gl_out tc */
    { uint32_t w[]={OP(15,9),2,id_main,0x6E69616Du,0x00000000u,id_glin,id_glout,id_tc};
      sb_pn(&b,w,9); }
    { uint32_t w[]={OP(16,3),id_main,4}; sb_pn(&b,w,3); } /* Triangles */
    { uint32_t w[]={OP(16,3),id_main,1}; sb_pn(&b,w,3); } /* SpacingEqual */
    { uint32_t w[]={OP(16,3),id_main,2}; sb_pn(&b,w,3); } /* VertexOrderCcw */

    /* Decorations */
    { uint32_t w[]={OP(71,3),id_PVin,2};   sb_pn(&b,w,3); } /* Block */
    { uint32_t w[]={OP(72,5),id_PVin,0,11,0}; sb_pn(&b,w,5); } /* Position */
    { uint32_t w[]={OP(71,3),id_PVout,2};  sb_pn(&b,w,3); } /* Block */
    { uint32_t w[]={OP(72,5),id_PVout,0,11,0}; sb_pn(&b,w,5); } /* Position */
    { uint32_t w[]={OP(71,4),id_tc,11,13}; sb_pn(&b,w,4); } /* BuiltIn TessCoord */

    /* Types */
    { uint32_t w[]={OP(19,2),id_void}; sb_pn(&b,w,2); }
    { uint32_t w[]={OP(21,4),id_int, 32,1}; sb_pn(&b,w,4); }
    { uint32_t w[]={OP(21,4),id_uint,32,0}; sb_pn(&b,w,4); }
    { uint32_t w[]={OP(22,3),id_float,32}; sb_pn(&b,w,3); }
    { uint32_t w[]={OP(23,4),id_v4,id_float,4}; sb_pn(&b,w,4); }
    { uint32_t w[]={OP(23,4),id_v3,id_float,3}; sb_pn(&b,w,4); }
    { uint32_t w[]={OP(43,4),id_uint,id_c32,32}; sb_pn(&b,w,4); }
    { uint32_t w[]={OP(30,3),id_PVin, id_v4}; sb_pn(&b,w,3); }
    { uint32_t w[]={OP(28,4),id_PVinarr,id_PVin,id_c32}; sb_pn(&b,w,4); }
    { uint32_t w[]={OP(30,3),id_PVout,id_v4}; sb_pn(&b,w,3); }
    { uint32_t w[]={OP(32,4),id_pIarr,1,id_PVinarr}; sb_pn(&b,w,4); }
    { uint32_t w[]={OP(32,4),id_pOPV, 3,id_PVout};   sb_pn(&b,w,4); }
    { uint32_t w[]={OP(32,4),id_pIv4, 1,id_v4};      sb_pn(&b,w,4); }
    { uint32_t w[]={OP(32,4),id_pOv4, 3,id_v4};      sb_pn(&b,w,4); }
    { uint32_t w[]={OP(32,4),id_pIv3, 1,id_v3};      sb_pn(&b,w,4); }
    { uint32_t w[]={OP(59,4),id_pIarr,id_glin, 1}; sb_pn(&b,w,4); }
    { uint32_t w[]={OP(59,4),id_pOPV, id_glout,3}; sb_pn(&b,w,4); }
    { uint32_t w[]={OP(59,4),id_pIv3, id_tc,   1}; sb_pn(&b,w,4); }
    { uint32_t w[]={OP(43,4),id_int,id_i0,0}; sb_pn(&b,w,4); }
    { uint32_t w[]={OP(43,4),id_int,id_i1,1}; sb_pn(&b,w,4); }
    { uint32_t w[]={OP(43,4),id_int,id_i2,2}; sb_pn(&b,w,4); }
    { uint32_t w[]={OP(33,3),id_fnty,id_void}; sb_pn(&b,w,3); }

    /* Function body */
    { uint32_t w[]={OP(54,5),id_void,id_main,0,id_fnty}; sb_pn(&b,w,5); }
    { uint32_t w[]={OP(248,2),id_lb}; sb_pn(&b,w,2); }
    { uint32_t w[]={OP(61,4),id_v3,id_tcv,id_tc}; sb_pn(&b,w,4); }
    { uint32_t w[]={OP(81,5),id_float,id_tcx,id_tcv,0}; sb_pn(&b,w,5); }
    { uint32_t w[]={OP(81,5),id_float,id_tcy,id_tcv,1}; sb_pn(&b,w,5); }
    { uint32_t w[]={OP(81,5),id_float,id_tcz,id_tcv,2}; sb_pn(&b,w,5); }
    /* AccessChain gl_in[0/1/2].Position */
    { uint32_t w[]={OP(65,6),id_pIv4,id_p0,id_glin,id_i0,id_i0}; sb_pn(&b,w,6); }
    { uint32_t w[]={OP(61,4),id_v4,id_in0,id_p0}; sb_pn(&b,w,4); }
    { uint32_t w[]={OP(65,6),id_pIv4,id_p1,id_glin,id_i1,id_i0}; sb_pn(&b,w,6); }
    { uint32_t w[]={OP(61,4),id_v4,id_in1,id_p1}; sb_pn(&b,w,4); }
    { uint32_t w[]={OP(65,6),id_pIv4,id_p2,id_glin,id_i2,id_i0}; sb_pn(&b,w,6); }
    { uint32_t w[]={OP(61,4),id_v4,id_in2,id_p2}; sb_pn(&b,w,4); }
    /* Barycentric: pos = x*in0 + y*in1 + z*in2 */
    { uint32_t w[]={OP(142,5),id_v4,id_s0,id_in0,id_tcx}; sb_pn(&b,w,5); }
    { uint32_t w[]={OP(142,5),id_v4,id_s1,id_in1,id_tcy}; sb_pn(&b,w,5); }
    { uint32_t w[]={OP(142,5),id_v4,id_s2,id_in2,id_tcz}; sb_pn(&b,w,5); }
    { uint32_t w[]={OP(129,5),id_v4,id_a01, id_s0,id_s1}; sb_pn(&b,w,5); }
    { uint32_t w[]={OP(129,5),id_v4,id_a012,id_a01,id_s2}; sb_pn(&b,w,5); }
    /* Store to gl_out.Position */
    { uint32_t w[]={OP(65,5),id_pOv4,id_op,id_glout,id_i0}; sb_pn(&b,w,5); }
    { uint32_t w[]={OP(62,3),id_op,id_a012}; sb_pn(&b,w,3); }
    { uint32_t w[]={OP(253,1)}; sb_pn(&b,w,1); }
    { uint32_t w[]={OP(56,1)};  sb_pn(&b,w,1); }

    b.w[3] = 41u; /* max ID = 40 */
    *out = b.w; *out_c = b.n;
    return true;
}