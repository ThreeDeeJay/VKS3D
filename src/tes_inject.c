/*
 * tes_inject.c — Build pass-through TCS and stereo TES SPIR-V at runtime
 *
 * On NVIDIA 426.06, gl_ViewIndex and gl_PositionPerViewNV are not populated
 * for VS or GS stages in multiview rendering.  They ARE populated for the
 * Tessellation Evaluation stage.
 *
 * Fix: for VS-only or VS+GS pipelines with TRIANGLE_LIST topology, inject a
 * level-1 pass-through TCS + stereo TES pair.  The TES uses the confirmed-
 * working gl_ViewIndex path to apply per-eye X offsets.
 *
 * TCS: passes gl_in[invoc_id].gl_Position → gl_out[invoc_id].gl_Position
 *      sets TessLevelInner[0..1] = TessLevelOuter[0..3] = 1.0
 *      (level-1 triangle tessellation = exact passthrough, no subdivision)
 *
 * TES: barycentric interpolation of the 3 control points, then applies
 *      gl_ViewIndex-based stereo X offset before writing gl_Position.
 *      The patcher (spirv_patch_stereo_vertex) handles the gl_ViewIndex
 *      injection into the base TES SPIR-V.
 *
 * SPIR-V is written directly as uint32_t words — no external compiler needed.
 *
 * BuiltIn values (from SPIR-V spec §3.21):
 *   Position        = 0    TessLevelOuter = 11
 *   InvocationId    = 8    TessLevelInner = 12
 *   Layer           = 9    TessCoord      = 13
 *   ViewportIndex   = 10   ViewIndex      = 4440
 *
 * Decoration values (from SPIR-V spec §3.20):
 *   Block = 2    BuiltIn = 11    Patch = 15
 *
 * OpCode values used:
 *   OpCapability=17  OpMemoryModel=14  OpEntryPoint=15  OpExecutionMode=16
 *   OpDecorate=71    OpMemberDecorate=72
 *   OpTypeVoid=19    OpTypeInt=21  OpTypeFloat=22  OpTypeVector=23
 *   OpTypeArray=28   OpTypeStruct=30  OpTypePointer=32  OpTypeFunction=33
 *   OpConstant=43    OpVariable=59    OpLoad=61    OpStore=62
 *   OpAccessChain=65 OpFAdd=129  OpFMul=133  OpVectorTimesScalar=142
 *   OpFunction=54    OpLabel=248  OpReturn=253  OpFunctionEnd=56
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "stereo_icd.h"

/* Reuse SpvBuf from shader.c — declare it locally */
typedef struct { uint32_t *w; size_t n, cap; } SpvBuf2;
static bool  sb2_init(SpvBuf2 *b, size_t c) { b->w=malloc(c*4); b->n=0; b->cap=c; return !!b->w; }
static bool  sb2_push(SpvBuf2 *b, uint32_t v) {
    if (b->n>=b->cap) { uint32_t *p=realloc(b->w,b->cap*8); if(!p)return false; b->w=p; b->cap*=2; }
    b->w[b->n++]=v; return true; }
static bool  sb2_push_n(SpvBuf2 *b, const uint32_t *v, size_t c) {
    for(size_t i=0;i<c;i++) if(!sb2_push(b,v[i])) return false; return true; }
static inline uint32_t op2(uint32_t op, uint32_t wc) { return (wc<<16)|op; }

/* ── Pass-through TCS SPIR-V ──────────────────────────────────────────────── *
 *
 * GLSL equivalent (3 control points, triangle mode, level 1):
 *
 *   layout(vertices = 3) out;
 *   void main() {
 *       gl_out[gl_InvocationID].gl_Position = gl_in[gl_InvocationID].gl_Position;
 *       gl_TessLevelOuter[0] = gl_TessLevelOuter[1] =
 *       gl_TessLevelOuter[2] = gl_TessLevelOuter[3] =
 *       gl_TessLevelInner[0] = gl_TessLevelInner[1] = 1.0;
 *   }
 *
 * Key SPIR-V decoration values:
 *   Block = 2,  BuiltIn = 11,  Patch = 15   (NOT 58 — common mistake)
 *
 * ID map:
 *   1=void  3=int  4=uint  5=float  6=v4float
 *   7=uint_32(const)  8=uint_3(const)  9=uint_2(const)  10=uint_4(const)
 *   11=PerVertex_in{v4float}   (INPUT  block — separate type from output)
 *   12=PerV_in_arr32           (gl_in array type)
 *   13=PerV_out_arr3           (gl_out array type)
 *   14=float_arr2  15=float_arr4
 *   16=ptr_Input_PerV_in_32   17=ptr_Output_PerV_out_3
 *   18=ptr_Input_v4  19=ptr_Output_v4
 *   20=ptr_Input_int  21=ptr_Output_float2  22=ptr_Output_float4
 *   23=ptr_Output_float
 *   24=gl_in  25=gl_out  26=tl_inner  27=tl_outer  28=invoc_id_var
 *   29=void_fn  30=main_fn
 *   31=int_0  32=int_1  33=int_2  34=int_3  35=float_1
 *   36=PerVertex_out{v4float}  (OUTPUT block — distinct type from input)
 *   SSA body: 37..49
 * ─────────────────────────────────────────────────────────────────────────── */
bool build_tcs_spv(uint32_t **out, size_t *out_c)
{
    SpvBuf2 b; if (!sb2_init(&b, 320)) return false;

    /* ── Header ─────────────────────────────────────────────────────────── */
    sb2_push(&b, 0x07230203u); /* magic */
    sb2_push(&b, 0x00010000u); /* SPIR-V 1.0 */
    sb2_push(&b, 0x56533344u); /* generator: "VKS3D" */
    sb2_push(&b, 55u);         /* bound (IDs up to 54) */
    sb2_push(&b, 0u);          /* schema */

    /* ── Capabilities ────────────────────────────────────────────────────── */
    { uint32_t w[]={op2(17,2), 1}; sb2_push_n(&b,w,2); }   /* Shader */
    { uint32_t w[]={op2(17,2), 34}; sb2_push_n(&b,w,2); }  /* Tessellation */

    /* ── Memory model ────────────────────────────────────────────────────── */
    { uint32_t w[]={op2(14,3), 0, 1}; sb2_push_n(&b,w,3); }

    /* ── Entry point ──────────────────────────────────────────────────────
     * TessellationControl(1), %main=30, "main", gl_in=24, gl_out=25,
     * tl_inner=26, tl_outer=27, invoc_id=28
     */
    { uint32_t w[]={op2(15,10), 1, 30, 0x6E69616Du, 0x00000000u, 24,25,26,27,28};
      sb2_push_n(&b,w,10); }

    /* ── Execution mode: OutputVertices 3 ────────────────────────────────── */
    /* SpvExecutionModeOutputVertices = 26 */
    { uint32_t w[]={op2(16,4), 30, 26, 3}; sb2_push_n(&b,w,4); }

    /* ── Decorations ─────────────────────────────────────────────────────── */
    /* INPUT block: PerVertex_in (ID 11) */
    { uint32_t w[]={op2(71,3), 11, 2}; sb2_push_n(&b,w,3); }       /* Block=2 */
    { uint32_t w[]={op2(72,5), 11, 0, 11, 0}; sb2_push_n(&b,w,5); }/* MemberDecorate 0 BuiltIn(11) Position(0) */
    /* OUTPUT block: PerVertex_out (ID 36) */
    { uint32_t w[]={op2(71,3), 36, 2}; sb2_push_n(&b,w,3); }       /* Block=2 */
    { uint32_t w[]={op2(72,5), 36, 0, 11, 0}; sb2_push_n(&b,w,5); }/* MemberDecorate 0 BuiltIn(11) Position(0) */
    /* TessLevelInner: BuiltIn(12) + Patch(15) */
    { uint32_t w[]={op2(71,4), 26, 11, 12}; sb2_push_n(&b,w,4); }  /* BuiltIn TessLevelInner */
    { uint32_t w[]={op2(71,3), 26, 15}; sb2_push_n(&b,w,3); }      /* Patch = 15 (NOT 58) */
    /* TessLevelOuter: BuiltIn(11) + Patch(15) */
    { uint32_t w[]={op2(71,4), 27, 11, 11}; sb2_push_n(&b,w,4); }  /* BuiltIn TessLevelOuter */
    { uint32_t w[]={op2(71,3), 27, 15}; sb2_push_n(&b,w,3); }      /* Patch = 15 */
    /* InvocationId */
    { uint32_t w[]={op2(71,4), 28, 11, 8}; sb2_push_n(&b,w,4); }   /* BuiltIn InvocationId */

    /* ── Types ───────────────────────────────────────────────────────────── */
    { uint32_t w[]={op2(19,2), 1}; sb2_push_n(&b,w,2); }            /* void */
    { uint32_t w[]={op2(21,4), 3, 32, 1}; sb2_push_n(&b,w,4); }    /* int32 signed */
    { uint32_t w[]={op2(21,4), 4, 32, 0}; sb2_push_n(&b,w,4); }    /* uint32 */
    { uint32_t w[]={op2(22,3), 5, 32}; sb2_push_n(&b,w,3); }       /* float32 */
    { uint32_t w[]={op2(23,4), 6, 5, 4}; sb2_push_n(&b,w,4); }     /* vec4 */

    /* Array size constants */
    { uint32_t w[]={op2(43,4), 4, 7, 32}; sb2_push_n(&b,w,4); }    /* uint=32 (gl_in max) */
    { uint32_t w[]={op2(43,4), 4, 8, 3}; sb2_push_n(&b,w,4); }     /* uint=3  (gl_out size) */
    { uint32_t w[]={op2(43,4), 4, 9, 2}; sb2_push_n(&b,w,4); }     /* uint=2  (tl_inner) */
    { uint32_t w[]={op2(43,4), 4, 10, 4}; sb2_push_n(&b,w,4); }    /* uint=4  (tl_outer) */

    /* INPUT struct: PerVertex_in {vec4} — for gl_in */
    { uint32_t w[]={op2(30,3), 11, 6}; sb2_push_n(&b,w,3); }
    /* OUTPUT struct: PerVertex_out {vec4} — for gl_out (SEPARATE type) */
    { uint32_t w[]={op2(30,3), 36, 6}; sb2_push_n(&b,w,3); }

    /* Array types */
    { uint32_t w[]={op2(28,4), 12, 11, 7}; sb2_push_n(&b,w,4); }   /* PerV_in[32] */
    { uint32_t w[]={op2(28,4), 13, 36, 8}; sb2_push_n(&b,w,4); }   /* PerV_out[3] */
    { uint32_t w[]={op2(28,4), 14, 5, 9}; sb2_push_n(&b,w,4); }    /* float[2] */
    { uint32_t w[]={op2(28,4), 15, 5, 10}; sb2_push_n(&b,w,4); }   /* float[4] */

    /* Pointer types */
    { uint32_t w[]={op2(32,4), 16, 1, 12}; sb2_push_n(&b,w,4); }   /* ptr_In_PerV_in_32 */
    { uint32_t w[]={op2(32,4), 17, 3, 13}; sb2_push_n(&b,w,4); }   /* ptr_Out_PerV_out_3 */
    { uint32_t w[]={op2(32,4), 18, 1, 6}; sb2_push_n(&b,w,4); }    /* ptr_In_v4 */
    { uint32_t w[]={op2(32,4), 19, 3, 6}; sb2_push_n(&b,w,4); }    /* ptr_Out_v4 */
    { uint32_t w[]={op2(32,4), 20, 1, 3}; sb2_push_n(&b,w,4); }    /* ptr_In_int */
    { uint32_t w[]={op2(32,4), 21, 3, 14}; sb2_push_n(&b,w,4); }   /* ptr_Out_float2 */
    { uint32_t w[]={op2(32,4), 22, 3, 15}; sb2_push_n(&b,w,4); }   /* ptr_Out_float4 */
    { uint32_t w[]={op2(32,4), 23, 3, 5}; sb2_push_n(&b,w,4); }    /* ptr_Out_float */

    /* Variables */
    { uint32_t w[]={op2(59,4), 16, 24, 1}; sb2_push_n(&b,w,4); }   /* gl_in  Input */
    { uint32_t w[]={op2(59,4), 17, 25, 3}; sb2_push_n(&b,w,4); }   /* gl_out Output */
    { uint32_t w[]={op2(59,4), 21, 26, 3}; sb2_push_n(&b,w,4); }   /* tl_inner Output */
    { uint32_t w[]={op2(59,4), 22, 27, 3}; sb2_push_n(&b,w,4); }   /* tl_outer Output */
    { uint32_t w[]={op2(59,4), 20, 28, 1}; sb2_push_n(&b,w,4); }   /* invoc_id Input */

    /* Integer index constants */
    { uint32_t w[]={op2(43,4), 3, 31, 0}; sb2_push_n(&b,w,4); }    /* int=0 */
    { uint32_t w[]={op2(43,4), 3, 32, 1}; sb2_push_n(&b,w,4); }    /* int=1 */
    { uint32_t w[]={op2(43,4), 3, 33, 2}; sb2_push_n(&b,w,4); }    /* int=2 */
    { uint32_t w[]={op2(43,4), 3, 34, 3}; sb2_push_n(&b,w,4); }    /* int=3 */
    { uint32_t w[]={op2(43,4), 5, 35, 0x3F800000u}; sb2_push_n(&b,w,4); } /* float=1.0 */

    /* Function type */
    { uint32_t w[]={op2(33,3), 29, 1}; sb2_push_n(&b,w,3); }       /* void() */

    /* ── Main function ──────────────────────────────────────────────────── */
    { uint32_t w[]={op2(54,5), 1, 30, 0, 29}; sb2_push_n(&b,w,5); }
    { uint32_t w[]={op2(248,2), 37}; sb2_push_n(&b,w,2); }          /* OpLabel */

    /* %38 = OpLoad int %invoc_id_var */
    { uint32_t w[]={op2(61,4), 3, 38, 28}; sb2_push_n(&b,w,4); }
    /* %39 = AccessChain ptr_In_v4 gl_in [38] [int_0] */
    { uint32_t w[]={op2(65,6), 18, 39, 24, 38, 31}; sb2_push_n(&b,w,6); }
    /* %40 = OpLoad v4 %39 */
    { uint32_t w[]={op2(61,4), 6, 40, 39}; sb2_push_n(&b,w,4); }
    /* %41 = AccessChain ptr_Out_v4 gl_out [38] [int_0] */
    { uint32_t w[]={op2(65,6), 19, 41, 25, 38, 31}; sb2_push_n(&b,w,6); }
    /* OpStore %41 %40 */
    { uint32_t w[]={op2(62,3), 41, 40}; sb2_push_n(&b,w,3); }

    /* TessLevelInner[0] = 1.0 */
    { uint32_t w[]={op2(65,5), 23, 42, 26, 31}; sb2_push_n(&b,w,5); }
    { uint32_t w[]={op2(62,3), 42, 35}; sb2_push_n(&b,w,3); }
    /* TessLevelInner[1] = 1.0 */
    { uint32_t w[]={op2(65,5), 23, 43, 26, 32}; sb2_push_n(&b,w,5); }
    { uint32_t w[]={op2(62,3), 43, 35}; sb2_push_n(&b,w,3); }
    /* TessLevelOuter[0] = 1.0 */
    { uint32_t w[]={op2(65,5), 23, 44, 27, 31}; sb2_push_n(&b,w,5); }
    { uint32_t w[]={op2(62,3), 44, 35}; sb2_push_n(&b,w,3); }
    /* TessLevelOuter[1] = 1.0 */
    { uint32_t w[]={op2(65,5), 23, 45, 27, 32}; sb2_push_n(&b,w,5); }
    { uint32_t w[]={op2(62,3), 45, 35}; sb2_push_n(&b,w,3); }
    /* TessLevelOuter[2] = 1.0 */
    { uint32_t w[]={op2(65,5), 23, 46, 27, 33}; sb2_push_n(&b,w,5); }
    { uint32_t w[]={op2(62,3), 46, 35}; sb2_push_n(&b,w,3); }
    /* TessLevelOuter[3] = 1.0 */
    { uint32_t w[]={op2(65,5), 23, 47, 27, 34}; sb2_push_n(&b,w,5); }
    { uint32_t w[]={op2(62,3), 47, 35}; sb2_push_n(&b,w,3); }

    { uint32_t w[]={op2(253,1)}; sb2_push_n(&b,w,1); }  /* OpReturn */
    { uint32_t w[]={op2(56,1)};  sb2_push_n(&b,w,1); }  /* OpFunctionEnd */

    /* Fix up the ID bound: max ID used = 47, so bound = 48 */
    b.w[3] = 48u;
    *out   = b.w;
    *out_c = b.n;
    return true;
}

/* ── Base TES SPIR-V (no stereo — patcher adds gl_ViewIndex offset) ─────── *
 *
 * GLSL equivalent (pass-through barycentric interpolation):
 *
 *   layout(triangles, equal_spacing, ccw) in;
 *   void main() {
 *       gl_Position = gl_TessCoord.x * gl_in[0].gl_Position
 *                   + gl_TessCoord.y * gl_in[1].gl_Position
 *                   + gl_TessCoord.z * gl_in[2].gl_Position;
 *   }
 *
 * The patcher (spirv_patch_stereo_vertex) is called on this SPIR-V to inject
 * the gl_ViewIndex-based stereo offset before OpReturn.
 *
 * gl_in[] element struct (ID 8) and gl_out struct (ID 10) are DISTINCT types.
 *
 * ID map:
 *   1=void  2=int  3=uint  4=float  5=v4float  6=v3float
 *   7=uint_32(const for gl_in array)
 *   8=PerVertex_in{v4float}  (INPUT  block — distinct from output)
 *   9=PerV_in_arr32
 *   10=PerVertex_out{v4float} (OUTPUT block — distinct from input)
 *   11=ptr_In_PerVarr32  12=ptr_Out_PerV
 *   13=ptr_In_v4  14=ptr_Out_v4  15=ptr_In_v3
 *   16=gl_in  17=gl_out  18=tess_coord_var
 *   19=void_fn  20=main_fn
 *   21=int_0  22=int_1  23=int_2
 *   SSA body: 24..40
 * ─────────────────────────────────────────────────────────────────────────── */
bool build_base_tes_spv(uint32_t **out, size_t *out_c)
{
    SpvBuf2 b; if (!sb2_init(&b, 260)) return false;

    /* Header */
    sb2_push(&b, 0x07230203u);
    sb2_push(&b, 0x00010000u);
    sb2_push(&b, 0x56533344u);
    sb2_push(&b, 41u);         /* bound: max ID = 40 */
    sb2_push(&b, 0u);

    /* Capabilities */
    { uint32_t w[]={op2(17,2), 1}; sb2_push_n(&b,w,2); }   /* Shader */
    { uint32_t w[]={op2(17,2), 34}; sb2_push_n(&b,w,2); }  /* Tessellation */

    /* OpMemoryModel Logical GLSL450 */
    { uint32_t w[]={op2(14,3), 0, 1}; sb2_push_n(&b,w,3); }

    /* OpEntryPoint TessellationEvaluation(2) %main "main" gl_in gl_out tess_coord */
    { uint32_t w[]={op2(15,9), 2, 20, 0x6E69616Du, 0x00000000u, 16,17,18};
      sb2_push_n(&b,w,9); }

    /* Execution modes */
    { uint32_t w[]={op2(16,3), 20, 4}; sb2_push_n(&b,w,3); }  /* Triangles */
    { uint32_t w[]={op2(16,3), 20, 1}; sb2_push_n(&b,w,3); }  /* SpacingEqual */
    { uint32_t w[]={op2(16,3), 20, 2}; sb2_push_n(&b,w,3); }  /* VertexOrderCcw */

    /* ── Decorations ──────────────────────────────────────────────────────── */
    /* INPUT PerVertex_in (ID 8): Block + member 0 = BuiltIn Position */
    { uint32_t w[]={op2(71,3), 8, 2}; sb2_push_n(&b,w,3); }
    { uint32_t w[]={op2(72,5), 8, 0, 11, 0}; sb2_push_n(&b,w,5); }
    /* OUTPUT PerVertex_out (ID 10): Block + member 0 = BuiltIn Position */
    { uint32_t w[]={op2(71,3), 10, 2}; sb2_push_n(&b,w,3); }
    { uint32_t w[]={op2(72,5), 10, 0, 11, 0}; sb2_push_n(&b,w,5); }
    /* TessCoord: BuiltIn TessCoord(13) */
    { uint32_t w[]={op2(71,4), 18, 11, 13}; sb2_push_n(&b,w,4); }

    /* ── Types ─────────────────────────────────────────────────────────────── */
    { uint32_t w[]={op2(19,2), 1}; sb2_push_n(&b,w,2); }            /* void */
    { uint32_t w[]={op2(21,4), 2, 32, 1}; sb2_push_n(&b,w,4); }    /* int32 */
    { uint32_t w[]={op2(21,4), 3, 32, 0}; sb2_push_n(&b,w,4); }    /* uint32 */
    { uint32_t w[]={op2(22,3), 4, 32}; sb2_push_n(&b,w,3); }       /* float32 */
    { uint32_t w[]={op2(23,4), 5, 4, 4}; sb2_push_n(&b,w,4); }     /* vec4 */
    { uint32_t w[]={op2(23,4), 6, 4, 3}; sb2_push_n(&b,w,4); }     /* vec3 */

    /* gl_in array size */
    { uint32_t w[]={op2(43,4), 3, 7, 32}; sb2_push_n(&b,w,4); }    /* uint=32 */

    /* INPUT struct */
    { uint32_t w[]={op2(30,3), 8, 5}; sb2_push_n(&b,w,3); }        /* PerVertex_in{vec4} */
    { uint32_t w[]={op2(28,4), 9, 8, 7}; sb2_push_n(&b,w,4); }     /* PerV_in[32] */
    /* OUTPUT struct (separate type) */
    { uint32_t w[]={op2(30,3), 10, 5}; sb2_push_n(&b,w,3); }       /* PerVertex_out{vec4} */

    /* Pointer types */
    { uint32_t w[]={op2(32,4), 11, 1, 9}; sb2_push_n(&b,w,4); }    /* ptr_In_PerVarr32 */
    { uint32_t w[]={op2(32,4), 12, 3, 10}; sb2_push_n(&b,w,4); }   /* ptr_Out_PerV */
    { uint32_t w[]={op2(32,4), 13, 1, 5}; sb2_push_n(&b,w,4); }    /* ptr_In_v4 */
    { uint32_t w[]={op2(32,4), 14, 3, 5}; sb2_push_n(&b,w,4); }    /* ptr_Out_v4 */
    { uint32_t w[]={op2(32,4), 15, 1, 6}; sb2_push_n(&b,w,4); }    /* ptr_In_v3 */

    /* Variables */
    { uint32_t w[]={op2(59,4), 11, 16, 1}; sb2_push_n(&b,w,4); }   /* gl_in Input */
    { uint32_t w[]={op2(59,4), 12, 17, 3}; sb2_push_n(&b,w,4); }   /* gl_out Output */
    { uint32_t w[]={op2(59,4), 15, 18, 1}; sb2_push_n(&b,w,4); }   /* tess_coord Input */

    /* Integer index constants */
    { uint32_t w[]={op2(43,4), 2, 21, 0}; sb2_push_n(&b,w,4); }    /* int=0 */
    { uint32_t w[]={op2(43,4), 2, 22, 1}; sb2_push_n(&b,w,4); }    /* int=1 */
    { uint32_t w[]={op2(43,4), 2, 23, 2}; sb2_push_n(&b,w,4); }    /* int=2 */

    /* Function type */
    { uint32_t w[]={op2(33,3), 19, 1}; sb2_push_n(&b,w,3); }       /* void() */

    /* ── Function body ──────────────────────────────────────────────────── */
    { uint32_t w[]={op2(54,5), 1, 20, 0, 19}; sb2_push_n(&b,w,5); }
    { uint32_t w[]={op2(248,2), 24}; sb2_push_n(&b,w,2); }          /* OpLabel */

    /* Load TessCoord (vec3) */
    { uint32_t w[]={op2(61,4), 6, 25, 18}; sb2_push_n(&b,w,4); }
    /* Extract x, y, z */
    { uint32_t w[]={op2(81,5), 4, 26, 25, 0}; sb2_push_n(&b,w,5); }
    { uint32_t w[]={op2(81,5), 4, 27, 25, 1}; sb2_push_n(&b,w,5); }
    { uint32_t w[]={op2(81,5), 4, 28, 25, 2}; sb2_push_n(&b,w,5); }

    /* Load gl_in[0].Position */
    { uint32_t w[]={op2(65,6), 13, 29, 16, 21, 21}; sb2_push_n(&b,w,6); }
    { uint32_t w[]={op2(61,4), 5, 30, 29}; sb2_push_n(&b,w,4); }
    /* Load gl_in[1].Position */
    { uint32_t w[]={op2(65,6), 13, 31, 16, 22, 21}; sb2_push_n(&b,w,6); }
    { uint32_t w[]={op2(61,4), 5, 32, 31}; sb2_push_n(&b,w,4); }
    /* Load gl_in[2].Position */
    { uint32_t w[]={op2(65,6), 13, 33, 16, 23, 21}; sb2_push_n(&b,w,6); }
    { uint32_t w[]={op2(61,4), 5, 34, 33}; sb2_push_n(&b,w,4); }

    /* Barycentric: pos = tc.x*in0 + tc.y*in1 + tc.z*in2 */
    { uint32_t w[]={op2(142,5), 5, 35, 30, 26}; sb2_push_n(&b,w,5); } /* VectorTimesScalar */
    { uint32_t w[]={op2(142,5), 5, 36, 32, 27}; sb2_push_n(&b,w,5); }
    { uint32_t w[]={op2(142,5), 5, 37, 34, 28}; sb2_push_n(&b,w,5); }
    { uint32_t w[]={op2(129,5), 5, 38, 35, 36}; sb2_push_n(&b,w,5); } /* FAdd */
    { uint32_t w[]={op2(129,5), 5, 39, 38, 37}; sb2_push_n(&b,w,5); }

    /* Store result to gl_out.Position (single struct, member 0) */
    { uint32_t w[]={op2(65,5), 14, 40, 17, 21}; sb2_push_n(&b,w,5); }
    { uint32_t w[]={op2(62,3), 40, 39}; sb2_push_n(&b,w,3); }

    { uint32_t w[]={op2(253,1)}; sb2_push_n(&b,w,1); }  /* OpReturn */
    { uint32_t w[]={op2(56,1)};  sb2_push_n(&b,w,1); }  /* OpFunctionEnd */

    /* Bound = 41 (max ID = 40) */
    b.w[3] = 41u;
    *out   = b.w;
    *out_c = b.n;
    return true;
}