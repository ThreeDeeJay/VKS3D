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
        uint32_t ifc = 4 + (uint32_t)(n_uvars*2); /* gl_in,gl_out,viv,gl_Layer(in pv_out) */
        sb_p(&b,OP(15,5+ifc)); sb_p(&b,3); sb_p(&b,id_main);
        sb_p(&b,0x6E69616Du); sb_p(&b,0x00000000u);
        sb_p(&b,id_gl_in); sb_p(&b,id_gl_out); sb_p(&b,id_viv);
        /* ViewIndex variable */
        for(int u=0;u<n_uvars;u++){sb_p(&b,id_uvi[u]);sb_p(&b,id_uvo[u]);}
    }
    /* ExecutionModes: Triangles=22, SpacingEqual=1, VertexOrderCcw=5 */
    {uint32_t w[]={OP(16,3),id_main,22};sb_pn(&b,w,3);}  /* Triangles */
    {uint32_t w[]={OP(16,3),id_main,1};sb_pn(&b,w,3);}   /* SpacingEqual */
    {uint32_t w[]={OP(16,3),id_main,5};sb_pn(&b,w,3);}   /* VertexOrderCcw */

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