
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <math.h>
#include "stereo_icd.h"
#include "tes_inject.h"
#include "spirv/unified1/spirv.h"
#define SpvExecVertex 0
#define SpvExecTessEval 2
#define SpvExecGeometry 3
#define SpvExecFragment 4
#define SpvExecMeshEXT 5365
#define SpvStorageOutput 3
#define SpvStorageInput 1
#define SPIRV_MAGIC 0x07230203u
typedef struct {
uint32_t *w;
size_t n;
size_t cap;
} SpvBuf;
static bool
sb_init(
SpvBuf *b,
size_t c)
{
b->w = malloc(c * sizeof(uint32_t));
b->n = 0;
b->cap = c;
return b->w != NULL;
}
static void
sb_free(
SpvBuf *b)
{
free(b->w);
b->w = NULL;
b->n = 0;
b->cap = 0;
}
static bool
sb_push(
SpvBuf *b,
uint32_t v)
{
if (b->n >= b->cap)
{
size_t new_cap = b->cap ? b->cap * 2 : 64;
uint32_t *p = realloc(
b->w,
new_cap * sizeof(uint32_t));
if (!p)
return false;
b->w = p;
b->cap = new_cap;
}
b->w[b->n++] = v;
return true;
}
static bool
sb_push_n(
SpvBuf *b,
const uint32_t *v,
size_t c)
{
for (size_t i = 0; i < c; i++)
{
if (!sb_push(b, v[i]))
return false;
}
return true;
}
static inline uint32_t
op_(
uint32_t op,
uint32_t wc)
{
return (wc << 16) | op;
}
typedef struct
{
const uint32_t *words;
size_t count;
uint32_t bound;
bool is_patchable;
bool has_mv_cap;
bool has_emit_vertex;
bool has_viewindex_builtin;
int exec_model;
uint32_t pos_var;
uint32_t pos_member_idx;
uint32_t pos_ptr_type;
bool pos_is_block;
uint32_t pos_block_type[8];
uint32_t pos_block_count;
uint32_t view_var;
uint32_t ft;
uint32_t v2t;
uint32_t v4t;
uint32_t it;
uint32_t ut;
uint32_t bt_type;
uint32_t bt;
uint32_t ptr_in_v2;
uint32_t ptr_out_v4;
uint32_t ptr_in_int;
uint32_t entry_function;
size_t entry_function_word;
size_t fn_word;
uint32_t position_function;
uint32_t emit_count;
uint32_t geometry_input_locations[32];
uint32_t geometry_input_vars[32];
uint32_t geometry_input_var_count;
uint32_t geometry_input_mode;
uint32_t geometry_output_mode;
uint32_t geometry_output_vertices;
uint32_t geometry_location_count;
uint32_t geometry_uniform_count;
uint32_t geometry_uniform_array_lengths[8];
bool geometry_has_image;
bool geometry_has_sampler;
uint32_t mesh_vertices_var;
uint32_t mesh_vertices_type;
uint32_t mesh_vertices_ptr_type;
uint32_t mesh_per_vertex_type;
uint32_t mesh_position_member;
bool mesh_position_found;
uint32_t mesh_vertices_array_type;
uint32_t dot_count;
bool has_matrix_ops;
bool has_vector_matrix_ops;
bool has_direct_position_write;
bool has_v2_position_input;
uint32_t location0_vars[16];
uint32_t location0_count;
uint32_t screen_values[16];
uint32_t screen_value_count;
bool screen_has_zw_use;
uint32_t value_capacity;
uint8_t *value_from_matrix;
uint8_t *is_matrix_type;
uint8_t *is_matrix_ptr;
uint32_t proj_struct_type;
uint32_t proj_ptr_type;
uint32_t proj_var;
uint32_t proj_set;
uint32_t proj_binding;
uint32_t proj_member_mask;
VkBool32 proj_found;
uint32_t proj_access_count;
uint32_t proj_load_count;
uint32_t proj_mtv_count;
uint8_t *is_proj_value;
uint8_t *is_view_value;
uint8_t *is_screen_value;
uint32_t screen_reconstruct_count;
} SpvMod;
static inline bool valid_id(const SpvMod *m, uint32_t id)
{
return id < m->value_capacity;
}
static inline uint8_t matrix_value(const SpvMod *m, uint32_t id)
{
return valid_id(m, id) ? m->value_from_matrix[id] : 0;
}
static inline void set_matrix_value(SpvMod *m, uint32_t id, uint8_t value)
{
if (valid_id(m, id))
m->value_from_matrix[id] = value;
}
static inline uint8_t matrix_ptr(const SpvMod *m, uint32_t id)
{
return valid_id(m, id) ? m->is_matrix_ptr[id] : 0;
}
static inline void set_matrix_ptr(SpvMod *m, uint32_t id, uint8_t value)
{
if (valid_id(m, id))
m->is_matrix_ptr[id] = value;
}
static inline uint8_t matrix_type(const SpvMod *m, uint32_t id)
{
return valid_id(m, id) ? m->is_matrix_type[id] : 0;
}
static inline void set_matrix_type(SpvMod *m, uint32_t id, uint8_t value)
{
if (valid_id(m, id))
m->is_matrix_type[id] = value;
}
static inline uint8_t proj_value(const SpvMod *m, uint32_t id)
{
return valid_id(m, id) ? m->is_proj_value[id] : 0;
}
static inline void set_proj_value(SpvMod *m, uint32_t id, uint8_t value)
{
if (valid_id(m, id))
m->is_proj_value[id] = value;
}
static inline uint8_t view_value(const SpvMod *m, uint32_t id)
{
return (id < m->value_capacity) ? m->is_view_value[id] : 0;
}
static inline void set_view_value(SpvMod *m, uint32_t id, uint8_t v)
{
if (id < m->value_capacity)
m->is_view_value[id] = v;
}
static inline uint8_t matrix_or2(const SpvMod *m,
uint32_t a,
uint32_t b)
{
return matrix_value(m, a) | matrix_value(m, b);
}
static void free_spv_provenance(SpvMod *m)
{
free(m->value_from_matrix);
free(m->is_matrix_type);
free(m->is_matrix_ptr);
free(m->is_proj_value);
free(m->is_view_value);
m->value_from_matrix = NULL;
m->is_matrix_type = NULL;
m->is_matrix_ptr = NULL;
m->is_proj_value = NULL;
m->is_view_value = NULL;
m->value_capacity = 0;
}
static uint64_t hash_spv(const uint32_t *data, size_t words);
static bool
spv_resolve_u32_constant(const SpvMod *m, uint32_t id, uint32_t *value)
{
if (!m || !value || !m->words)
return false;
for (size_t i = 5; i < m->count; )
{
uint32_t op = m->words[i] & 0xffffu;
uint32_t wc = m->words[i] >> 16;
if (!wc || i + wc > m->count)
break;
if (op == SpvOpConstant && wc >= 4 && m->words[i + 2] == id)
{
*value = m->words[i + 3];
return true;
}
i += wc;
}
return false;
}
static const char *
spv_op_name(uint32_t op);
static bool is_location0_var(const SpvMod *m,uint32_t id)
{
for(uint32_t n=0;n<m->location0_count;n++)
{
if(m->location0_vars[n]==id)
return true;
}
return false;
}
static bool is_screen_value(const SpvMod *m,uint32_t id)
{
for(uint32_t n=0;n<m->screen_value_count;n++)
{
if(m->screen_values[n]==id)
return true;
}
return false;
}
static void do_scan(SpvMod *m, bool p2)
{
const uint32_t *w=m->words;
uint32_t current_function = 0;
if(!p2)
{
m->geometry_location_count=0;
m->geometry_input_var_count=0;
}
#define MAT(id) matrix_value(m, (id))
#define SETMAT(id,v) set_matrix_value(m, (id), (v))
#define PTR(id) matrix_ptr(m, (id))
#define SETPTR(id,v) set_matrix_ptr(m, (id), (v))
#define TYPE(id) matrix_type(m, (id))
#define SETTYPE(id,v) set_matrix_type(m, (id), (v))
#define PROJ(id) proj_value(m, (id))
#define SETPROJ(id,v) set_proj_value(m, (id), (v))
#define VIEW(id) view_value(m, (id))
#define SETVIEW(id,v) set_view_value(m, (id), (v))
for (size_t i=5;i<m->count;) {
uint32_t op=w[i]&0xffff, wc=w[i]>>16;
if (!wc||i+wc>m->count) break;
if (!p2)
{
switch(op) {
case SpvOpDot:
m->dot_count++;
break;
case SpvOpAccessChain:
case SpvOpInBoundsAccessChain:
case SpvOpPtrAccessChain:
if (wc >= 4 &&
w[i + 2] < m->value_capacity &&
w[i + 3] < m->value_capacity)
{
SETPTR(w[i + 2], PTR(w[i + 3]));
}
if (wc >= 5 &&
w[i+3] == m->proj_var)
{
uint32_t member_id = w[i + 4];
uint32_t member_value = member_id;
(void)spv_resolve_u32_constant(
m,
member_id,
&member_value);
m->proj_access_count++;
m->proj_found = VK_TRUE;
switch (member_value)
{
case 0: 
case 1: 
case 2: 
case 3: 
case 4: 
case 5: 
SETPROJ(
w[i + 2],
member_value + 1);
if (member_value == 2)
SETVIEW(
w[i + 2],
1);
break;
default:
break;
}
}
break;
case SpvOpLoad:
if (wc >= 4 &&
w[i + 2] < m->value_capacity &&
w[i + 3] < m->value_capacity)
{
SETMAT(
w[i + 2],
MAT(w[i + 3]) || PTR(w[i + 3]));
}
if(wc>=4&&is_location0_var(m,w[i+3]))
{
if(m->screen_value_count<16)
m->screen_values[m->screen_value_count++]=w[i+2];
}
if (wc >= 4)
{
if (PROJ(w[i + 3]))
{
SETPROJ(w[i + 2], PROJ(w[i + 3]));
m->proj_load_count++;
}
if (VIEW(w[i + 3]))
{
SETVIEW(w[i + 2], VIEW(w[i + 3]));
}
}
break;
case SpvOpCompositeExtract:
if (wc >= 5 &&
w[i + 2] < m->value_capacity &&
w[i + 3] < m->value_capacity)
{
SETMAT(w[i + 2], MAT(w[i + 3]));
if (PROJ(w[i + 3]))
{
SETPROJ(w[i + 2], PROJ(w[i + 3]));
}
if (VIEW(w[i + 3]))
SETVIEW(w[i + 2], VIEW(w[i + 3]));
if (VIEW(w[i + 3]) != 0)
SETVIEW(w[i + 2], VIEW(w[i + 3]));
}
break;
case SpvOpVectorShuffle:
if (wc >= 6 &&
w[i + 2] < m->value_capacity &&
w[i + 3] < m->value_capacity)
{
SETMAT(w[i + 2], MAT(w[i + 3]));
if (PROJ(w[i + 3]))
SETPROJ(w[i + 2], PROJ(w[i + 3]));
if (VIEW(w[i + 3]))
SETVIEW(w[i + 2], VIEW(w[i + 3]));
}
if(wc>=9&&(is_screen_value(m,w[i+3])||is_screen_value(m,w[i+4]))&&(w[i+5]>=2||w[i+6]>=2||w[i+7]>=2||w[i+8]>=2))
{
m->screen_has_zw_use=true;
}
break;
case SpvOpCompositeConstruct:
if (wc >= 5 &&
w[i + 2] < m->value_capacity)
{
uint8_t matrix = 0;
uint8_t proj = 0;
uint8_t view = 0;
for (uint32_t k = 3; k < wc; ++k)
{
uint32_t id = w[i + k];
if (id >= m->value_capacity)
continue;
matrix |= MAT(id);
if (!proj && PROJ(id))
proj = PROJ(id);
if (!view && VIEW(id))
view = VIEW(id);
}
SETMAT(w[i + 2], matrix);
if (proj)
SETPROJ(w[i + 2], proj);
if (view)
SETVIEW(w[i + 2], view);
}
break;
case SpvOpCapability:
if(wc>=2&&w[i+1]==SpvCapabilityMultiView) m->has_mv_cap=true;
break;
case SpvOpEntryPoint:
if(wc>=3){
uint32_t e=w[i+1];
if(e==SpvExecVertex||e==SpvExecTessEval||e==SpvExecGeometry||e==SpvExecMeshEXT)
{
m->is_patchable=true;
m->exec_model=(int)e;
m->entry_function = w[i+2];
}
else if (e == SpvExecutionModelRayGenerationKHR)
{
m->exec_model = (int)e;
m->entry_function = w[i + 2];
}
}
break;
case SpvOpExecutionMode:
if(wc>=3&&w[i+1]==m->entry_function)
{
if(w[i+2]==SpvExecutionModeInputPoints)m->geometry_input_mode=w[i+2];
else if(w[i+2]==SpvExecutionModeOutputTriangleStrip)m->geometry_output_mode=w[i+2];
else if(w[i+2]==SpvExecutionModeOutputVertices&&wc>=4)m->geometry_output_vertices=w[i+3];
}
break;
case SpvOpTypeFloat:
if(wc==3&&w[i+2]==32) m->ft=w[i+1];
break;
case SpvOpTypeVector:
if(wc==4&&w[i+2]==m->ft)
{
if(w[i+3]==2)
m->v2t=w[i+1];
else if(w[i+3]==4)
m->v4t=w[i+1];
}
break;
case SpvOpTypeInt:
if (wc == 4 && w[i + 2] == 32)
{
if (w[i + 3] == 1)
{
m->it = w[i + 1];
}
else
{
m->ut = w[i + 1];
}
}
break;
case SpvOpTypeBool:
if (wc >= 2 && !m->bt_type)
m->bt_type = w[i + 1];
break;
case SpvOpTypeMatrix:
if (wc >= 4)
{
if (w[i + 1] < m->value_capacity)
SETTYPE(w[i + 1], 1);
}
break;
case SpvOpTypeStruct:
if (wc >= 3)
{
uint8_t matrix = 0;
for (uint32_t k = 2; k < wc; ++k)
{
if (w[i + k] < m->value_capacity &&
TYPE(w[i + k]))
{
matrix = 1;
break;
}
}
if (w[i + 1] < m->value_capacity)
SETTYPE(w[i + 1], matrix);
if (matrix)
{
m->proj_struct_type = w[i+1];
}
}
break;
case SpvOpTypeArray:
if (wc >= 4)
{
if (m->exec_model == SpvExecMeshEXT &&
w[i + 2] == m->mesh_per_vertex_type)
{
m->mesh_vertices_type = w[i + 1];
}
}
break;
case SpvOpTypeRuntimeArray:
break;
case SpvOpTranspose:
if (wc >= 4 &&
w[i + 2] < m->value_capacity &&
w[i + 3] < m->value_capacity)
{
SETMAT(w[i + 2], MAT(w[i + 3]));
if (PROJ(w[i + 3]))
SETPROJ(w[i + 2], PROJ(w[i + 3]));
if (VIEW(w[i + 3]))
SETVIEW(w[i + 2], VIEW(w[i + 3]));
}
break;
case SpvOpMatrixTimesVector:
case SpvOpMatrixTimesMatrix:
m->has_matrix_ops = true;
case SpvOpVectorTimesScalar:
case SpvOpVectorTimesMatrix:
case SpvOpMatrixTimesScalar:
if (op == SpvOpVectorTimesMatrix)
m->has_vector_matrix_ops = true;
if (wc >= 5)
{
if (w[i + 2] < m->value_capacity)
{
uint32_t a = w[i + 3];
uint32_t b = w[i + 4];
uint8_t proj_a = PROJ(a);
uint8_t proj_b = PROJ(b);
uint8_t view_a = VIEW(a);
uint8_t view_b = VIEW(b);
if (op == SpvOpMatrixTimesVector || op == SpvOpMatrixTimesMatrix)
{
}
if ((proj_a || proj_b) &&
(op == SpvOpMatrixTimesVector ||
op == SpvOpMatrixTimesMatrix))
{
uint8_t proj = proj_a ? proj_a : proj_b;
m->proj_member_mask |= 1u << (proj - 1);
m->proj_mtv_count++;
}
if (view_a || view_b)
SETVIEW(w[i + 2], view_a ? view_a : view_b);
SETMAT(w[i + 2], 1);
}
}
break;
case SpvOpCopyObject:
case SpvOpBitcast:
if (wc >= 4 &&
w[i + 2] < m->value_capacity &&
w[i + 3] < m->value_capacity)
{
SETMAT(w[i + 2], MAT(w[i + 3]));
if (PROJ(w[i + 3]))
SETPROJ(w[i + 2], PROJ(w[i + 3]));
if (VIEW(w[i + 3]))
SETVIEW(w[i + 2], VIEW(w[i + 3]));
}
break;
case SpvOpExtInst:
if (wc >= 7 &&
w[i + 2] < m->value_capacity)
{
uint8_t matrix = 0;
uint8_t proj = 0;
uint8_t view = 0;
for (uint32_t k = 5; k < wc; ++k)
{
uint32_t id = w[i + k];
if (id >= m->value_capacity)
continue;
matrix |= MAT(id);
if (!proj && PROJ(id))
proj = PROJ(id);
if (!view && VIEW(id))
view = VIEW(id);
}
SETMAT(w[i + 2], matrix);
if (proj)
SETPROJ(w[i + 2], proj);
if (view)
SETVIEW(w[i + 2], view);
}
break;
case SpvOpFAdd:
case SpvOpFSub:
case SpvOpFMul:
case SpvOpFDiv:
if (wc >= 5 &&
w[i + 2] < m->value_capacity)
{
SETMAT(
w[i + 2],
matrix_or2(m, w[i + 4], w[i + 5]));
if (PROJ(w[i + 4]))
SETPROJ(w[i + 2], PROJ(w[i + 4]));
else if (PROJ(w[i + 5]))
SETPROJ(w[i + 2], PROJ(w[i + 5]));
if (VIEW(w[i + 4]))
SETVIEW(w[i + 2], VIEW(w[i + 4]));
else if (VIEW(w[i + 5]))
SETVIEW(w[i + 2], VIEW(w[i + 5]));
}
break;
case SpvOpSelect:
if (wc >= 6 &&
w[i + 2] < m->value_capacity)
{
SETMAT(
w[i + 2],
matrix_or2(m, w[i + 4], w[i + 5]));
if (PROJ(w[i + 4]))
SETPROJ(w[i + 2], PROJ(w[i + 4]));
else if (PROJ(w[i + 5]))
SETPROJ(w[i + 2], PROJ(w[i + 5]));
if (VIEW(w[i + 4]))
SETVIEW(w[i + 2], VIEW(w[i + 4]));
else if (VIEW(w[i + 5]))
SETVIEW(w[i + 2], VIEW(w[i + 5]));
}
break;
case SpvOpFunctionCall:
break;
case SpvOpCompositeInsert:
if (wc >= 6 &&
w[i + 2] < m->value_capacity)
{
uint8_t matrix = 0;
uint8_t proj = 0;
uint8_t view = 0;
if (w[i + 3] < m->value_capacity)
{
matrix |= MAT(w[i + 3]);
if (PROJ(w[i + 3]))
proj = PROJ(w[i + 3]);
if (VIEW(w[i + 3]))
view = VIEW(w[i + 3]);
}
if (w[i + 4] < m->value_capacity)
{
matrix |= MAT(w[i + 4]);
if (!proj && PROJ(w[i + 4]))
proj = PROJ(w[i + 4]);
if (!view && VIEW(w[i + 4]))
view = VIEW(w[i + 4]);
}
SETMAT(w[i + 2], matrix);
if (proj)
SETPROJ(w[i + 2], proj);
if (view)
SETVIEW(w[i + 2], view);
}
break;
case SpvOpTypePointer:
if (wc >= 4)
{
if (TYPE(w[i + 3]))
{
SETPTR(w[i + 1], 1);
}
if (w[i+3] == m->proj_struct_type)
{
m->proj_ptr_type = w[i+1];
}
if (m->exec_model == SpvExecMeshEXT &&
w[i + 2] == SpvStorageOutput &&
w[i + 3] == m->mesh_vertices_type)
{
m->mesh_vertices_ptr_type = w[i + 1];
}
if (w[i + 2] == SpvStorageOutput &&
m->v4t &&
w[i + 3] == m->v4t)
{
m->ptr_out_v4 = w[i + 1];
if (m->exec_model == SpvExecMeshEXT &&
w[i + 2] == SpvStorageOutput &&
m->mesh_vertices_type &&
w[i + 3] == m->mesh_vertices_type)
{
m->mesh_vertices_var = w[i + 2];
m->mesh_vertices_ptr_type = w[i + 1];
}
}
if (w[i + 2] == SpvStorageInput)
{
if (m->v2t &&
w[i + 3] == m->v2t)
{
m->ptr_in_v2 = w[i + 1];
}
if (m->it &&
w[i + 3] == m->it)
{
m->ptr_in_int = w[i + 1];
}
}
}
break;
case SpvOpVariable:
if (wc >= 4 &&
w[i + 1] < m->value_capacity &&
w[i + 2] < m->value_capacity &&
PTR(w[i + 1]))
{
SETPTR(w[i + 2], 1);
}
if (m->exec_model == SpvExecMeshEXT &&
m->mesh_vertices_type &&
w[i + 3] == SpvStorageOutput &&
PTR(w[i + 1]))
{
if (w[i + 1] == m->pos_ptr_type ||
w[i + 1] == m->mesh_vertices_ptr_type)
{
}
}
if (w[i+1] == m->proj_ptr_type &&
w[i+3] == SpvStorageClassUniform)
{
m->proj_var = w[i+2];
}
if (m->exec_model == SpvExecMeshEXT &&
w[i + 3] == SpvStorageOutput &&
w[i + 1] == m->mesh_vertices_ptr_type)
{
m->mesh_vertices_var = w[i + 2];
}
if (w[i + 3] == SpvStorageInput)
{
if (m->ptr_in_v2 &&
w[i + 1] == m->ptr_in_v2)
{
m->has_v2_position_input = true;
}
}
break;
case SpvOpDecorate:
if (wc >= 4)
{
if (w[i+2] == SpvDecorationDescriptorSet &&
w[i+1] == m->proj_var)
{
m->proj_set = w[i+3];
}
if (w[i+2] == SpvDecorationBinding &&
w[i+1] == m->proj_var)
{
m->proj_binding = w[i+3];
}
}
if(wc>=4&&w[i+2]==SpvDecorationLocation&&w[i+3]==0)
{
if(m->location0_count<16)
m->location0_vars[m->location0_count++]=w[i+1];
}
if(wc>=4&&w[i+2]==SpvDecorationLocation&&m->exec_model==SpvExecGeometry&&m->geometry_input_var_count<32)
{
m->geometry_input_vars[m->geometry_input_var_count++]=w[i+1];
}
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
bool known_block = false;
for (uint32_t k = 0; k < m->pos_block_count; ++k)
{
if (m->pos_block_type[k] == w[i+1])
known_block = true;
}
if (!known_block && m->pos_block_count < 8)
m->pos_block_type[m->pos_block_count++] = w[i+1];
m->pos_member_idx = w[i+2];
m->pos_is_block = true;
m->pos_var = 0;
m->pos_ptr_type = 0;
if (m->exec_model == SpvExecMeshEXT)
{
m->mesh_per_vertex_type = w[i+1];
m->mesh_position_member = w[i+2];
m->mesh_position_found = true;
}
}
break;
case SpvOpFunction:
if (!m->fn_word)
m->fn_word = i;
if (wc >= 3)
current_function = w[i+2];
break;
case SpvOpFunctionEnd:
current_function = 0;
break;
case SpvOpEmitVertex:
m->emit_count++;
m->has_emit_vertex = true;
break;
case SpvOpStore:
if (wc >= 3 &&
w[i + 1] == m->pos_var)
{
if (current_function &&
!m->position_function)
{
m->position_function = current_function;
}
uint32_t source = w[i + 2];
if (source >= m->value_capacity ||
!MAT(source))
{
m->has_direct_position_write = true;
}
}
break;
}
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
if(m->exec_model==SpvExecGeometry)
{
}
}
static void count_geometry_inputs(SpvMod *m)
{
const uint32_t *w=m->words;
uint32_t input_count=0;
uint32_t location_count=0;
uint32_t variable_ops=0;
uint32_t decorate_ops=0;
uint32_t input_vars=0;
uint32_t location_decorations=0;
m->geometry_location_count=0;
uint32_t locations[32];
for(size_t i=5;i<m->count;)
{
uint32_t op=w[i]&0xffff,wc=w[i]>>16;
if(!wc||i+wc>m->count)break;
if(op==SpvOpDecorate&&wc>=4&&w[i+2]==SpvDecorationLocation&&location_count<32)
locations[location_count++]=w[i+1];
i+=wc;
}
for(size_t i=5;i<m->count;)
{
uint32_t op=w[i]&0xffff,wc=w[i]>>16;
if(!wc||i+wc>m->count)break;
if(op==SpvOpVariable&&wc>=4&&w[i+3]==SpvStorageInput)
{
for(uint32_t k=0;k<location_count;k++)
{
if(locations[k]==w[i+2])
{
m->geometry_location_count++;
break;
}
}
}
i+=wc;
}
}
static void spv_scan(SpvMod *m)
{
do_scan(m, false);
do_scan(m, false);
if (m->pos_is_block)
{
do_scan(m, true);
}
if(m->exec_model==SpvExecGeometry)
count_geometry_inputs(m);
}
uint64_t hash_spv(const uint32_t *data, size_t words)
{
uint64_t h = 1469598103934665603ULL; // FNV offset basis
for (size_t i = 0; i < words; i++) {
h ^= data[i];
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
StereoDevice *
stereo_device_from_command_buffer(
VkCommandBuffer cb)
{
extern StereoDevice g_devices[];
extern uint32_t g_device_count;
for (uint32_t d = 0; d < g_device_count; d++)
{
StereoDevice *sd = &g_devices[d];
for (uint32_t i = 0; i < sd->cb_track_count; i++)
{
if (sd->cb_track[i].cb == cb)
return sd;
}
}
return NULL;
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
CHECK_ARRAY_COUNT(sd->cb_track_count, MAX_CB_TRACK, "cb_track_count");
uint32_t idx = sd->cb_track_count++;
sd->cb_track[idx].cb = cb;
sd->cb_track[idx].pipeline = pipe;
sd->cb_track[idx].render_pass = VK_NULL_HANDLE;
sd->cb_track[idx].framebuffer = VK_NULL_HANDLE;
sd->cb_track[idx].subpass = 0;
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
CHECK_ARRAY_COUNT(sd->cb_track_count, MAX_CB_TRACK, "cb_track_count");
uint32_t idx = sd->cb_track_count++;
sd->cb_track[idx].cb = cb;
sd->cb_track[idx].pipeline = VK_NULL_HANDLE;
sd->cb_track[idx].render_pass = rp;
sd->cb_track[idx].framebuffer = VK_NULL_HANDLE;
sd->cb_track[idx].subpass = subpass;
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
if (sd->pipeline_info_count >= sd->pipeline_info_capacity)
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
CHECK_ARRAY_COUNT(sd->pipeline_info_count, sd->pipeline_info_capacity, "pipeline_info_count");
StereoPipelineInfo *info =
&sd->pipeline_info[sd->pipeline_info_count++];
memset(info, 0, sizeof(*info));
return info;
}
typedef struct {
SpvMod *m;
bool have_view;
bool has_projection_path;
bool has_view_path;
uint32_t uv4;
uint32_t uint_;
uint32_t ut;
uint32_t bt;
uint32_t cz;
uint32_t cf0;
uint32_t cf1;
uint32_t cl;
uint32_t cr;
uint32_t cc;
uint32_t projection_mode;
uint32_t bg_expand;
float lo_dbg;
float ro_dbg;
bool force_far_depth;
bool procedural_sky;
StereoDebugCtx *dbg;
} BodyCtx;
typedef struct StereoDebugCtx {
uint32_t pipeline_index;
VkRenderPass render_pass;
bool is_multiview;
uint32_t stage;
uint32_t vertex_binding_count;
uint32_t is_quad;
VkBool32 has_proj_ubo;
uint32_t proj_set;
uint32_t proj_binding;
uint32_t proj_member_mask;
uint32_t proj_var;
bool has_matrix_ops;
bool direct_position_write;
} StereoDebugCtx;
static uint32_t
spirv_get_result_id(uint32_t op, const uint32_t *ins, uint32_t wc)
{
if (!ins)
return 0;
if (op == SpvOpLabel ||
op == SpvOpTypeVoid ||
op == SpvOpTypeBool ||
op == SpvOpTypeInt ||
op == SpvOpTypeFloat ||
op == SpvOpTypeVector ||
op == SpvOpTypeMatrix ||
op == SpvOpTypeImage ||
op == SpvOpTypeSampler ||
op == SpvOpTypeSampledImage ||
op == SpvOpTypeArray ||
op == SpvOpTypeRuntimeArray ||
op == SpvOpTypeStruct ||
op == SpvOpTypeOpaque ||
op == SpvOpTypePointer ||
op == SpvOpTypeFunction)
{
return wc >= 2 ? ins[1] : 0;
}
if (op == SpvOpConstantTrue ||
op == SpvOpConstantFalse ||
op == SpvOpConstant ||
op == SpvOpConstantComposite ||
op == SpvOpConstantNull ||
op == SpvOpSpecConstantTrue ||
op == SpvOpSpecConstantFalse ||
op == SpvOpSpecConstant ||
op == SpvOpSpecConstantComposite ||
op == SpvOpVariable ||
op == SpvOpLoad ||
op == SpvOpAccessChain ||
op == SpvOpInBoundsAccessChain ||
op == SpvOpPtrAccessChain ||
op == SpvOpCompositeConstruct ||
op == SpvOpCompositeExtract ||
op == SpvOpCompositeInsert ||
op == SpvOpCopyObject ||
op == SpvOpTranspose ||
op == SpvOpVectorExtractDynamic ||
op == SpvOpVectorInsertDynamic ||
op == SpvOpVectorShuffle ||
op == SpvOpMatrixTimesScalar ||
op == SpvOpVectorTimesScalar ||
op == SpvOpMatrixTimesVector ||
op == SpvOpVectorTimesMatrix ||
op == SpvOpMatrixTimesMatrix ||
op == SpvOpIAddCarry ||
op == SpvOpISubBorrow ||
op == SpvOpUMulExtended ||
op == SpvOpSMulExtended ||
op == SpvOpSNegate ||
op == SpvOpFNegate ||
op == SpvOpIAdd ||
op == SpvOpFAdd ||
op == SpvOpISub ||
op == SpvOpFSub ||
op == SpvOpIMul ||
op == SpvOpFMul ||
op == SpvOpUDiv ||
op == SpvOpSDiv ||
op == SpvOpFDiv ||
op == SpvOpUMod ||
op == SpvOpSRem ||
op == SpvOpSMod ||
op == SpvOpFRem ||
op == SpvOpFMod ||
op == SpvOpVectorTimesScalar ||
op == SpvOpShiftRightLogical ||
op == SpvOpShiftRightArithmetic ||
op == SpvOpShiftLeftLogical ||
op == SpvOpBitwiseOr ||
op == SpvOpBitwiseXor ||
op == SpvOpBitwiseAnd ||
op == SpvOpNot ||
op == SpvOpBitFieldInsert ||
op == SpvOpBitFieldSExtract ||
op == SpvOpBitFieldUExtract ||
op == SpvOpBitReverse ||
op == SpvOpBitCount ||
op == SpvOpAny ||
op == SpvOpAll ||
op == SpvOpIsNan ||
op == SpvOpIsInf ||
op == SpvOpIsFinite ||
op == SpvOpIsNormal ||
op == SpvOpSignBitSet ||
op == SpvOpLessOrGreater ||
op == SpvOpOrdered ||
op == SpvOpUnordered ||
op == SpvOpLogicalEqual ||
op == SpvOpLogicalNotEqual ||
op == SpvOpLogicalOr ||
op == SpvOpLogicalAnd ||
op == SpvOpLogicalNot ||
op == SpvOpSelect ||
op == SpvOpIEqual ||
op == SpvOpINotEqual ||
op == SpvOpUGreaterThan ||
op == SpvOpSGreaterThan ||
op == SpvOpUGreaterThanEqual ||
op == SpvOpSGreaterThanEqual ||
op == SpvOpULessThan ||
op == SpvOpSLessThan ||
op == SpvOpULessThanEqual ||
op == SpvOpSLessThanEqual ||
op == SpvOpFOrdEqual ||
op == SpvOpFUnordEqual ||
op == SpvOpFOrdNotEqual ||
op == SpvOpFUnordNotEqual ||
op == SpvOpFOrdLessThan ||
op == SpvOpFUnordLessThan ||
op == SpvOpFOrdGreaterThan ||
op == SpvOpFUnordGreaterThan ||
op == SpvOpFOrdLessThanEqual ||
op == SpvOpFUnordLessThanEqual ||
op == SpvOpFOrdGreaterThanEqual ||
op == SpvOpFUnordGreaterThanEqual ||
op == SpvOpConvertSToF ||
op == SpvOpConvertUToF ||
op == SpvOpFConvert ||
op == SpvOpQuantizeToF16 ||
op == SpvOpConvertFToS ||
op == SpvOpConvertFToU ||
op == SpvOpSatConvertSToU ||
op == SpvOpSatConvertUToS ||
op == SpvOpConvertPtrToU ||
op == SpvOpConvertUToPtr ||
op == SpvOpBitcast ||
op == SpvOpSConvert ||
op == SpvOpUConvert ||
op == SpvOpImageSampleImplicitLod ||
op == SpvOpImageSampleExplicitLod ||
op == SpvOpImageSampleDrefImplicitLod ||
op == SpvOpImageSampleDrefExplicitLod ||
op == SpvOpImageSampleProjImplicitLod ||
op == SpvOpImageSampleProjExplicitLod ||
op == SpvOpImageSampleProjDrefImplicitLod ||
op == SpvOpImageSampleProjDrefExplicitLod ||
op == SpvOpImageFetch ||
op == SpvOpImageGather ||
op == SpvOpImageDrefGather ||
op == SpvOpImageRead ||
op == SpvOpImage ||
op == SpvOpImageQueryFormat ||
op == SpvOpImageQueryOrder ||
op == SpvOpImageQuerySizeLod ||
op == SpvOpImageQuerySize ||
op == SpvOpImageQueryLod ||
op == SpvOpImageQueryLevels ||
op == SpvOpImageQuerySamples ||
op == SpvOpSampledImage ||
op == SpvOpFunction ||
op == SpvOpFunctionParameter ||
op == SpvOpFunctionCall ||
op == SpvOpExtInst ||
op == SpvOpPhi)
{
return wc >= 3 ? ins[2] : 0;
}
return 0;
}
static uint32_t spirv_get_value_type(
const uint32_t *in,
size_t in_c,
uint32_t value_id)
{
if (!in || !value_id)
return 0;
for (size_t i = 5; i < in_c;)
{
uint32_t wc = in[i] >> 16;
uint32_t op = in[i] & 0xffff;
if (!wc || i + wc > in_c)
break;
if (spirv_get_result_id(op, &in[i], wc) == value_id)
{
if (wc >= 3 &&
op != SpvOpLabel &&
op != SpvOpFunction)
return in[i + 1];
return 0;
}
i += wc;
}
return 0;
}
static uint32_t
spirv_get_pointer_pointee_type(
const uint32_t *in,
size_t in_c,
uint32_t id)
{
uint32_t pointer_type = 0;
if (!in || in_c < 5 || !id)
return 0;
for (size_t i = 5; i < in_c;)
{
uint32_t opx = in[i] & 0xffff;
uint32_t wcx = in[i] >> 16;
if (!wcx || i + wcx > in_c)
break;
if (wcx >= 3 &&
in[i + 2] == id &&
(opx == SpvOpVariable ||
opx == SpvOpAccessChain ||
opx == SpvOpInBoundsAccessChain ||
opx == SpvOpPtrAccessChain ||
opx == SpvOpArrayLength ||
opx == SpvOpImageTexelPointer))
{
pointer_type = in[i + 1];
break;
}
i += wcx;
}
if (!pointer_type)
return 0;
for (size_t i = 5; i < in_c;)
{
uint32_t opx = in[i] & 0xffff;
uint32_t wcx = in[i] >> 16;
if (!wcx || i + wcx > in_c)
break;
if (opx == SpvOpTypePointer &&
wcx >= 4 &&
in[i + 1] == pointer_type)
{
return in[i + 3];
}
i += wcx;
}
return 0;
}
static bool
spirv_is_vector_type(
const uint32_t *in,
size_t in_c,
uint32_t type_id,
uint32_t component_type,
uint32_t component_count)
{
if (!in || in_c < 5 || !type_id)
return false;
for (size_t i = 5; i < in_c;)
{
uint32_t opx = in[i] & 0xffff;
uint32_t wcx = in[i] >> 16;
if (!wcx || i + wcx > in_c)
break;
if (opx == SpvOpTypeVector &&
wcx >= 4 &&
in[i + 1] == type_id)
return in[i + 2] == component_type &&
in[i + 3] == component_count;
i += wcx;
}
return false;
}
static uint32_t spirv_find_vector_type(
const uint32_t *in,
size_t in_c,
uint32_t component_type,
uint32_t component_count)
{
if (!in || in_c < 5 || !component_type || !component_count)
return 0;
for (size_t i=5; i < in_c;)
{
uint32_t wc=in[i] >> 16;
uint32_t op=in[i] & 0xffff;
if (!wc || i + wc > in_c)
break;
if (op == SpvOpTypeVector &&
wc >= 4 &&
in[i + 2] == component_type &&
in[i + 3] == component_count)
return in[i + 1];
i += wc;
}
return 0;
}
static void emit_body(SpvBuf *out, const BodyCtx *c, uint32_t *nid)
{
SpvMod *m = c->m;
uint32_t ch = (*nid)++;
uint32_t lp = (*nid)++;
uint32_t pptr;
if (!m->pos_var || !m->v4t || !m->ft)
return;
if (m->pos_is_block && (!c->uv4 || !m->it || !c->cz))
return;
if (m->pos_is_block)
{
uint32_t mid = (m->pos_member_idx == 0) ? c->cz : (*nid)++;
if (m->pos_member_idx != 0)
{
uint32_t ci[] = {
op_(SpvOpConstant, 4),
m->it,
mid,
m->pos_member_idx
};
sb_push_n(out, ci, 4);
}
uint32_t a[] = {
op_(SpvOpAccessChain, 5),
c->uv4,
ch,
m->pos_var,
mid
};
sb_push_n(out, a, 5);
pptr = ch;
}
else
{
pptr = m->pos_var;
}
{
uint32_t w[] = {
op_(SpvOpLoad, 4),
m->v4t,
lp,
pptr
};
sb_push_n(out, w, 4);
}
uint32_t lv = c->have_view ? (*nid)++ : 0;
uint32_t isl = c->have_view ? (*nid)++ : 0;
uint32_t sel = (*nid)++;
uint32_t px = (*nid)++;
uint32_t nx = (*nid)++;
uint32_t nx2 = (*nid)++;
uint32_t np = (*nid)++;
uint32_t pw = c->force_far_depth ? (*nid)++ : 0;
uint32_t np_far = c->force_far_depth ? (*nid)++ : 0;
if (c->have_view && m->view_var && m->it && c->bt)
{
{
uint32_t w[] = {
op_(SpvOpLoad, 4),
m->it,
lv,
m->view_var
};
sb_push_n(out, w, 4);
}
{
uint32_t w[] = {
op_(SpvOpIEqual, 5),
c->bt,
isl,
lv,
c->cz
};
sb_push_n(out, w, 5);
}
{
uint32_t w[] = {
op_(SpvOpSelect, 6),
m->ft,
sel,
isl,
c->cr,
c->cl
};
sb_push_n(out, w, 6);
}
}
else
{
sel = c->cl;
}
{
uint32_t w[] = {
op_(SpvOpCompositeExtract, 5),
m->ft,
px,
lp,
0u
};
sb_push_n(out, w, 5);
}
if (c->force_far_depth)
{
uint32_t w[] = {
op_(SpvOpCompositeExtract, 5),
m->ft,
pw,
lp,
3u
};
sb_push_n(out, w, 5);
}
if (c->force_far_depth)
{
uint32_t bg_offset = (*nid)++;
uint32_t bg_x = (*nid)++;
uint32_t w[] = {
op_(SpvOpFMul, 5),
m->ft,
bg_offset,
sel,
c->cc
};
sb_push_n(out, w, 5);
if (c->procedural_sky)
{
uint32_t bg_scale = (*nid)++;
{
uint32_t w2[] = {
op_(SpvOpFAdd, 5),
m->ft,
bg_scale,
c->bg_expand,
c->cf1
};
sb_push_n(out, w2, 5);
}
{
uint32_t w3[] = {
op_(SpvOpFMul, 5),
m->ft,
bg_x,
px,
bg_scale
};
sb_push_n(out, w3, 5);
}
{
uint32_t w4[] = {
op_(SpvOpFSub, 5),
m->ft,
nx2,
bg_x,
bg_offset
};
sb_push_n(out, w4, 5);
}
}
else
{
if (!m->has_matrix_ops)
{
uint32_t bg_scale = (*nid)++;
{
uint32_t w2[] = {
op_(SpvOpFAdd, 5),
m->ft,
bg_scale,
c->bg_expand,
c->cf1
};
sb_push_n(out, w2, 5);
}
{
uint32_t w3[] = {
op_(SpvOpFMul, 5),
m->ft,
bg_x,
px,
bg_scale
};
sb_push_n(out, w3, 5);
}
{
uint32_t w4[] = {
op_(SpvOpFSub, 5),
m->ft,
nx2,
bg_x,
bg_offset
};
sb_push_n(out, w4, 5);
}
}
else
{
{
uint32_t w2[] = {
op_(SpvOpFMul, 5),
m->ft,
bg_x,
bg_offset,
pw
};
sb_push_n(out, w2, 5);
}
{
uint32_t w3[] = {
op_(SpvOpFSub, 5),
m->ft,
nx2,
px,
bg_x
};
sb_push_n(out, w3, 5);
}
}
}
}
else
{
uint32_t pw = (*nid)++;
uint32_t convmag = (*nid)++;
uint32_t tmp = (*nid)++;
{
uint32_t w[] = {
op_(SpvOpCompositeExtract, 5),
m->ft,
pw,
lp,
3u
};
sb_push_n(out, w, 5);
}
{
uint32_t w[] = {
op_(SpvOpFMul, 5),
m->ft,
convmag,
pw,
c->cc
};
sb_push_n(out, w, 5);
}
{
uint32_t w[] = {
op_(SpvOpFMul, 5),
m->ft,
tmp,
sel,
convmag
};
sb_push_n(out, w, 5);
}
{
uint32_t w[] = {
op_(SpvOpFAdd, 5),
m->ft,
nx,
px,
sel
};
sb_push_n(out, w, 5);
}
{
uint32_t w[] = {
op_(SpvOpFSub, 5),
m->ft,
nx2,
nx,
tmp
};
sb_push_n(out, w, 5);
}
}
{
uint32_t w[] = {
op_(SpvOpCompositeInsert, 6),
m->v4t,
np,
nx2,
lp,
0u
};
sb_push_n(out, w, 6);
}
uint32_t final_pos = np;
if (c->force_far_depth)
{
uint32_t w[] = {
op_(SpvOpCompositeInsert, 6),
m->v4t,
np_far,
pw,
np,
2u
};
sb_push_n(out, w, 6);
final_pos = np_far;
}
{
uint32_t w[] = {
op_(SpvOpStore, 3),
pptr,
final_pos
};
sb_push_n(out, w, 3);
}
}
static bool emit_mesh_position_adjust(
SpvBuf *out,
SpvMod *m,
uint32_t *nid,
uint32_t pos,
uint32_t *new_pos,
uint32_t cl,
uint32_t cr,
uint32_t cc,
uint32_t cz,
int projection_mode,
bool have_view,
uint32_t bt,
float lo,
float ro,
float conv)
{
uint32_t lv = 0;
uint32_t isl = 0;
uint32_t sel = 0;
uint32_t px = (*nid)++;
uint32_t nx = (*nid)++;
uint32_t nx2 = (*nid)++;
uint32_t np = (*nid)++;
if (have_view)
{
lv = (*nid)++;
isl = (*nid)++;
sel = (*nid)++;
{
uint32_t w[] = {
op_(SpvOpLoad, 4),
m->it,
lv,
m->view_var
};
sb_push_n(out, w, 4);
}
{
uint32_t w[] = {
op_(SpvOpIEqual, 5),
bt,
isl,
lv,
cz
};
sb_push_n(out, w, 5);
}
{
uint32_t w[] = {
op_(SpvOpSelect, 6),
m->ft,
sel,
isl,
cr,
cl
};
sb_push_n(out, w, 6);
}
}
else
{
sel = cl;
}
{
uint32_t w[] = {
op_(SpvOpCompositeExtract, 5),
m->ft,
px,
pos,
0u
};
sb_push_n(out, w, 5);
}
uint32_t pw = (*nid)++;
uint32_t convmag = (*nid)++;
uint32_t tmp = (*nid)++;
{
uint32_t w[] = {
op_(SpvOpCompositeExtract, 5),
m->ft,
pw,
pos,
3u
};
sb_push_n(out, w, 5);
}
{
uint32_t w[] = {
op_(SpvOpFMul, 5),
m->ft,
convmag,
pw,
cc
};
sb_push_n(out, w, 5);
}
{
uint32_t w[] = {
op_(SpvOpFMul, 5),
m->ft,
tmp,
sel,
convmag
};
sb_push_n(out, w, 5);
}
{
uint32_t w[] = {
op_(SpvOpFAdd, 5),
m->ft,
nx,
px,
sel
};
sb_push_n(out, w, 5);
}
{
uint32_t w[] = {
op_(SpvOpFSub, 5),
m->ft,
nx2,
nx,
tmp
};
sb_push_n(out, w, 5);
}
{
uint32_t w[] = {
op_(SpvOpCompositeInsert, 6),
m->v4t,
np,
nx2,
pos,
0u
};
sb_push_n(out, w, 6);
}
*new_pos = np;
return true;
}
static bool stereo_skip_shader_patch(uint64_t spv_hash)
{
static bool skip_list_init;
static bool skip_all;
static char skip_list[1024];
if (!skip_list_init)
{
const char *env =
stereo_getenv("VKS3D_SKIP_SHADER_PATCHES");
if (env)
{
strncpy(skip_list, env, sizeof(skip_list) - 1);
skip_list[sizeof(skip_list) - 1] = '\0';
skip_all = strchr(skip_list, '*') != NULL;
}
skip_list_init = true;
}
if (!skip_list[0])
return false;
if (skip_all)
{
return true;
}
char hashstr[17];
snprintf(
hashstr,
sizeof(hashstr),
"%016llx",
(unsigned long long)spv_hash);
if (strstr(skip_list, hashstr))
{
return true;
}
return false;
}
bool spirv_patch_stereo_mesh(
const StereoConfig *cfg,
const uint32_t *in,
size_t in_c,
uint32_t **out,
size_t *out_c,
float lo,
float ro,
float conv,
bool inj_vi,
StereoDebugCtx *dbg)
{
if (!in || in_c < 5 || in[0] != SPIRV_MAGIC)
return false;
SpvMod m = {0};
m.words = in;
m.count = in_c;
m.bound = m.words[3];
m.value_capacity = m.bound + 128;
m.value_from_matrix =
calloc(m.value_capacity, sizeof(uint8_t));
m.is_matrix_type =
calloc(m.value_capacity, sizeof(uint8_t));
m.is_matrix_ptr =
calloc(m.value_capacity, sizeof(uint8_t));
m.is_proj_value =
calloc(m.value_capacity, sizeof(uint8_t));
m.is_view_value =
calloc(m.value_capacity, sizeof(uint8_t));
if (!m.value_from_matrix ||
!m.is_matrix_type ||
!m.is_matrix_ptr ||
!m.is_proj_value ||
!m.is_view_value)
{
free_spv_provenance(&m);
return false;
}
spv_scan(&m);
for (size_t di = 5; di < in_c;)
{
uint32_t dop = in[di] & 0xffff;
uint32_t dwc = in[di] >> 16;
if (!dwc || di + dwc > in_c)
break;
if (dop == SpvOpDecorate && dwc >= 4)
{
if (in[di + 2] == SpvDecorationBuiltIn)
{
}
}
if (dop == SpvOpVariable && dwc >= 4)
{
}
else if (dop == SpvOpMemberDecorate && dwc >= 5)
{
if (in[di + 3] == SpvDecorationBuiltIn)
{
}
}
else if (dop == SpvOpTypeStruct && dwc >= 2)
{
}
else if (dop == SpvOpAccessChain && dwc >= 5)
{
if (m.exec_model == SpvExecMeshEXT &&
m.mesh_vertices_var &&
in[di + 3] == m.mesh_vertices_var)
{
uint32_t member_id = in[di + dwc - 1];
uint32_t member_value = member_id;
bool resolved =
spv_resolve_u32_constant(
&m,
member_id,
&member_value);
}
}
else if (dop == SpvOpStore && dwc >= 3)
{
}
di += dwc;
}
if (dbg)
{
dbg->has_matrix_ops = m.has_matrix_ops;
dbg->direct_position_write =
m.has_direct_position_write;
dbg->has_proj_ubo = false;
if (m.proj_found)
{
dbg->has_proj_ubo = true;
dbg->proj_set = m.proj_set;
dbg->proj_binding = m.proj_binding;
dbg->proj_member_mask =
m.proj_member_mask;
dbg->proj_var = m.proj_var;
}
}
if (m.exec_model != SpvExecMeshEXT)
{
free_spv_provenance(&m);
return false;
}
if (!m.mesh_vertices_var ||
!m.mesh_position_found)
{
free_spv_provenance(&m);
return false;
}
int projection_mode =
cfg ? cfg->projection :
STEREO_PROJECTION_PARALLEL;
uint32_t nid = m.bound;
uint32_t id_ptr_int = nid++;
uint32_t id_new_it = 0;
if (!m.it && inj_vi && !m.view_var)
{
id_new_it = nid++;
m.it = id_new_it;
}
bool will_inj_vi =
inj_vi &&
!m.view_var &&
m.it;
uint32_t id_inj_view =
will_inj_vi ? nid++ : 0;
bool have_view =
m.view_var ||
will_inj_vi;
uint32_t id_new_bt = 0;
if (!m.bt &&
!m.bt_type &&
have_view &&
m.it)
{
id_new_bt = nid++;
}
uint32_t id_cz = nid++;
uint32_t id_cl = nid++;
uint32_t id_cr = nid++;
uint32_t id_cc = nid++;
uint32_t uint_ptr =
m.ptr_in_int ?
m.ptr_in_int :
id_ptr_int;
uint32_t bt =
m.bt ?
m.bt :
(m.bt_type ?
m.bt_type :
id_new_bt);
SpvBuf ann;
SpvBuf te;
SpvBuf ob;
if (!sb_init(&ann, 16) ||
!sb_init(&te, 128) ||
!sb_init(&ob, in_c + 256))
{
sb_free(&ann);
sb_free(&te);
sb_free(&ob);
free_spv_provenance(&m);
return false;
}
if (id_new_it)
{
uint32_t w[] = {
op_(SpvOpTypeInt, 4),
id_new_it,
32,
0
};
sb_push_n(&te, w, 4);
}
if (m.it && !m.ptr_in_int)
{
uint32_t w[] = {
op_(SpvOpTypePointer, 4),
id_ptr_int,
SpvStorageInput,
m.it
};
sb_push_n(&te, w, 4);
m.ptr_in_int = id_ptr_int;
}
if (id_new_bt)
{
uint32_t w[] = {
op_(SpvOpTypeBool, 2),
id_new_bt
};
sb_push_n(&te, w, 2);
}
if (m.it)
{
uint32_t w[] = {
op_(SpvOpConstant, 4),
m.it,
id_cz,
0
};
sb_push_n(&te, w, 4);
}
{
uint32_t w[] = {
op_(SpvOpConstant, 4),
m.ft,
id_cl,
0
};
memcpy(&w[3], &lo, sizeof(lo));
sb_push_n(&te, w, 4);
}
{
uint32_t w[] = {
op_(SpvOpConstant, 4),
m.ft,
id_cr,
0
};
memcpy(&w[3], &ro, sizeof(ro));
sb_push_n(&te, w, 4);
}
{
uint32_t w[] = {
op_(SpvOpConstant, 4),
m.ft,
id_cc,
0
};
memcpy(&w[3], &conv, sizeof(conv));
sb_push_n(&te, w, 4);
}
if (will_inj_vi)
{
uint32_t d[] = {
op_(SpvOpDecorate, 4),
id_inj_view,
SpvDecorationBuiltIn,
SpvBuiltInViewIndex
};
sb_push_n(&ann, d, 4);
uint32_t v[] = {
op_(SpvOpVariable, 4),
uint_ptr,
id_inj_view,
SpvStorageInput
};
sb_push_n(&te, v, 4);
m.view_var = id_inj_view;
}
size_t ins_ann = 0;
size_t ins_t = 0;
bool in_entry_function = false;
bool patched_position = false;
for (size_t i = 5; i < in_c;)
{
uint32_t opx = in[i] & 0xffff;
uint32_t wcx = in[i] >> 16;
if (!wcx || i + wcx > in_c)
break;
if (!ins_ann &&
(opx == SpvOpTypeVoid ||
opx == SpvOpTypeBool ||
opx == SpvOpTypeInt ||
opx == SpvOpTypeFloat ||
opx == SpvOpTypeVector ||
opx == SpvOpTypeMatrix ||
opx == SpvOpTypeImage ||
opx == SpvOpTypeSampler ||
opx == SpvOpTypeSampledImage ||
opx == SpvOpTypeArray ||
opx == SpvOpTypeRuntimeArray ||
opx == SpvOpTypeStruct ||
opx == SpvOpTypeOpaque ||
opx == SpvOpTypePointer ||
opx == SpvOpTypeFunction ||
opx == SpvOpTypeForwardPointer ||
opx == SpvOpConstantTrue ||
opx == SpvOpConstantFalse ||
opx == SpvOpConstant ||
opx == SpvOpConstantComposite ||
opx == SpvOpVariable))
{
ins_ann = i;
}
if (opx == SpvOpFunction)
{
in_entry_function =
wcx >= 4 &&
in[i + 2] == m.entry_function;
if (in_entry_function)
ins_t = i;
}
if (in_entry_function &&
opx == SpvOpFunctionEnd)
{
break;
}
i += wcx;
}
if (!ins_t)
{
sb_free(&ann);
sb_free(&te);
sb_free(&ob);
free_spv_provenance(&m);
return false;
}
bool need_mv_cap =
id_inj_view &&
!m.has_mv_cap;
bool mv_done = false;
bool ann_done = false;
bool te_done = false;
bool ext_done = false;
uint32_t spv_version = in[1];
bool need_mv_ext =
need_mv_cap &&
((spv_version >> 16) == 1) &&
(((spv_version >> 8) & 0xff) == 0);
sb_push_n(&ob, in, 5);
for (size_t i = 5; i < in_c;)
{
if (!mv_done && need_mv_cap)
{
uint32_t c[] = {
op_(SpvOpCapability, 2),
SpvCapabilityMultiView
};
sb_push_n(&ob, c, 2);
mv_done = true;
}
if (!ann_done && i == ins_ann)
{
sb_push_n(&ob, ann.w, ann.n);
ann_done = true;
}
if (!te_done && i == ins_t)
{
sb_push_n(&ob, te.w, te.n);
te_done = true;
}
uint32_t opx = in[i] & 0xffff;
uint32_t wcx = in[i] >> 16;
if (!wcx || i + wcx > in_c)
break;
if (!ext_done &&
need_mv_ext &&
opx != SpvOpCapability)
{
uint32_t e[] = {
op_(SpvOpExtension, 6),
0x5F565053,
0x5F52484B,
0x746C756D,
0x65697669,
0x00000077
};
sb_push_n(&ob, e, 6);
ext_done = true;
}
if (id_inj_view &&
opx == SpvOpEntryPoint &&
wcx >= 4 &&
in[i + 1] == SpvExecMeshEXT &&
in[i + 2] == m.entry_function)
{
sb_push(
&ob,
((wcx + 1) << 16) |
SpvOpEntryPoint);
sb_push_n(
&ob,
&in[i + 1],
wcx - 1);
sb_push(
&ob,
id_inj_view);
i += wcx;
continue;
}
if (in_entry_function &&
opx == SpvOpStore &&
wcx >= 3)
{
uint32_t ptr = in[i + 1];
uint32_t value = in[i + 2];
bool position_store = false;
size_t scan = 5;
while (scan < i)
{
uint32_t so = in[scan] & 0xffff;
uint32_t sw = in[scan] >> 16;
if (!sw || scan + sw > i)
break;
if (so == SpvOpAccessChain && sw >= 5)
{
if (m.exec_model == SpvExecMeshEXT)
{
if (m.mesh_vertices_var &&
m.mesh_position_found &&
sw >= 5 &&
in[scan + 2] == ptr &&
in[scan + 3] == m.mesh_vertices_var)
{
uint32_t member_id = in[scan + sw - 1];
uint32_t member_value = member_id;
if (spv_resolve_u32_constant(
&m,
member_id,
&member_value) &&
member_value == m.mesh_position_member)
{
position_store = true;
}
}
}
else if (in[scan + 3] == m.pos_var &&
in[scan + sw - 1] == 0)
{
position_store = true;
}
}
scan += sw;
}
if (position_store)
{
uint32_t np = 0;
emit_mesh_position_adjust(
&ob,
&m,
&nid,
value,
&np,
id_cl,
id_cr,
id_cc,
id_cz,
projection_mode,
have_view,
bt,
lo,
ro,
conv);
uint32_t w[] = {
op_(SpvOpStore, 3),
ptr,
np
};
sb_push_n(&ob, w, 3);
patched_position = true;
i += wcx;
continue;
}
}
sb_push_n(&ob, &in[i], wcx);
i += wcx;
}
if (!ext_done && need_mv_ext)
{
uint32_t e[] = {
op_(SpvOpExtension, 6),
0x5F565053,
0x5F52484B,
0x746C756D,
0x65697669,
0x00000077
};
sb_push_n(&ob, e, 6);
}
if (!ann_done)
sb_push_n(&ob, ann.w, ann.n);
if (!te_done)
sb_push_n(&ob, te.w, te.n);
if (!patched_position)
{
sb_free(&ann);
sb_free(&te);
sb_free(&ob);
free_spv_provenance(&m);
return false;
}
ob.w[3] = nid;
*out = ob.w;
*out_c = ob.n;
sb_free(&ann);
sb_free(&te);
free_spv_provenance(&m);
return true;
}
bool spirv_patch_stereo_vertex(
const StereoConfig *cfg,
const uint32_t *in,
size_t in_c,
uint32_t **out,
size_t *out_c,
float lo,
float ro,
float conv,
bool inj_vi,
bool force_far_depth,
bool procedural_sky,
StereoDebugCtx *dbg)
{
if (!in || in_c < 5 || in[0] != SPIRV_MAGIC)
return false;
int projection_mode =
cfg ? cfg->projection : STEREO_PROJECTION_PARALLEL;
SpvMod m = {0};
m.words = in;
m.count = in_c;
m.bound = m.words[3];
m.value_capacity = m.bound + 64;
m.value_from_matrix =
calloc(m.value_capacity, sizeof(uint8_t));
m.is_matrix_type =
calloc(m.value_capacity, sizeof(uint8_t));
m.is_matrix_ptr =
calloc(m.value_capacity, sizeof(uint8_t));
m.is_proj_value =
calloc(m.value_capacity, sizeof(uint8_t));
m.is_view_value =
calloc(m.value_capacity, sizeof(uint8_t));
if (!m.value_from_matrix ||
!m.is_matrix_type ||
!m.is_matrix_ptr ||
!m.is_proj_value ||
!m.is_view_value)
{
free_spv_provenance(&m);
return false;
}
spv_scan(&m);
uint64_t spv_hash = hash_spv(m.words, m.count);
if(stereo_skip_shader_patch(spv_hash))
{
free_spv_provenance(&m);
return false;
}
if(cfg&&cfg->mono_ui)
{
bool ui_candidate=(m.exec_model==SpvExecVertex&&m.pos_is_block&&!m.has_matrix_ops&&!m.has_vector_matrix_ops&&m.has_v2_position_input&&m.dot_count<=2&&!m.screen_has_zw_use&&!m.has_emit_vertex&&m.screen_value_count<=4)||(m.exec_model==SpvExecVertex&&m.location0_count>0&&m.screen_value_count>0&&!m.screen_has_zw_use&&!m.has_emit_vertex&&m.has_direct_position_write)||(m.exec_model==SpvExecVertex&&m.proj_found&&m.dot_count==2&&m.location0_count>0&&m.screen_value_count>0&&!m.has_emit_vertex&&!m.has_matrix_ops&&!m.has_vector_matrix_ops)||(m.exec_model==SpvExecVertex&&m.proj_found&&m.dot_count==2&&m.location0_count>0&&m.screen_value_count>0&&m.screen_has_zw_use&&!m.has_emit_vertex&&m.has_matrix_ops);
if(ui_candidate)
{
free_spv_provenance(&m);
return false;
}
}
if (dbg && !dbg->is_multiview)
{
free_spv_provenance(&m);
return false;
}
if (m.proj_found)
{
}
if (dbg)
{
dbg->has_matrix_ops = m.has_matrix_ops;
dbg->direct_position_write = m.has_direct_position_write;
dbg->has_proj_ubo = false;
if (m.proj_found)
{
dbg->has_proj_ubo = true;
dbg->proj_set = m.proj_set;
dbg->proj_binding = m.proj_binding;
dbg->proj_member_mask = m.proj_member_mask;
dbg->proj_var = m.proj_var;
}
}
if (m.exec_model == SpvExecVertex)
{
if (!m.pos_var)
{
free_spv_provenance(&m);
return false;
}
}
if (!m.is_patchable)
{
free_spv_provenance(&m);
return false;
}
bool is_gs =
(m.exec_model == SpvExecGeometry);
if (is_gs)
{
}
uint32_t nid = m.bound;
uint32_t id_ptr_v4 = nid++;
uint32_t id_ptr_int = nid++;
uint32_t id_new_it = 0;
if (!m.it && inj_vi && !m.view_var)
{
id_new_it = nid++;
m.it = id_new_it;
}
bool will_inj_vi =
inj_vi &&
!m.view_var &&
m.it;
uint32_t id_inj_view =
will_inj_vi ? nid++ : 0;
bool have_view =
m.view_var ||
will_inj_vi;
uint32_t id_new_bt = 0;
if (!m.bt &&
!m.bt_type &&
have_view &&
m.it)
{
id_new_bt = nid++;
}
uint32_t bt_for_types =
m.bt ?
m.bt :
(m.bt_type ? m.bt_type : id_new_bt);
uint32_t id_v4bt = 0;
bool new_v4bt = false;
if (bt_for_types)
id_v4bt = spirv_find_vector_type(in,in_c,bt_for_types,4);
if (!id_v4bt && m.v4t && bt_for_types)
{
id_v4bt = nid++;
new_v4bt = true;
}
uint32_t id_cz = nid++;
uint32_t id_cf0 = nid++;
uint32_t id_cf1 = nid++;
uint32_t id_cl = nid++;
uint32_t id_cr = nid++;
uint32_t id_cc = nid++;
uint32_t id_bg_expand = nid++;
uint32_t uv4 =
m.ptr_out_v4 ?
m.ptr_out_v4 :
id_ptr_v4;
uint32_t uint_ =
m.ptr_in_int ?
m.ptr_in_int :
id_ptr_int;
uint32_t bt =
m.bt ?
m.bt :
(m.bt_type ? m.bt_type : id_new_bt);
SpvBuf ann;
SpvBuf te;
if (!sb_init(&ann, 16) || !sb_init(&te, 96))
{
free_spv_provenance(&m);
return false;
}
if (id_new_it)
{
uint32_t w[] =
{
op_(SpvOpTypeInt, 4),
id_new_it,
32,
1
};
sb_push_n(&te, w, 4);
}
if (!m.ptr_out_v4)
{
uint32_t w[] =
{
op_(SpvOpTypePointer, 4),
id_ptr_v4,
SpvStorageOutput,
m.v4t
};
sb_push_n(&te, w, 4);
}
if (m.it && !m.ptr_in_int)
{
uint32_t w[] =
{
op_(SpvOpTypePointer, 4),
id_ptr_int,
SpvStorageInput,
m.it
};
sb_push_n(&te, w, 4);
m.ptr_in_int = id_ptr_int;
uint_ = id_ptr_int;
}
if (id_new_bt)
{
uint32_t w[] =
{
op_(SpvOpTypeBool, 2),
id_new_bt
};
sb_push_n(&te, w, 2);
}
if (new_v4bt)
{
uint32_t w[] =
{
op_(SpvOpTypeVector, 4),
id_v4bt,
bt_for_types,
4
};
sb_push_n(&te, w, 4);
}
if (m.it)
{
uint32_t w[] =
{
op_(SpvOpConstant, 4),
m.it,
id_cz,
0
};
sb_push_n(&te, w, 4);
}
{
uint32_t w[4] =
{
op_(SpvOpConstant, 4),
m.ft,
id_cf0,
0
};
float z = 0.0f;
memcpy(&w[3], &z, sizeof(z));
sb_push_n(&te, w, 4);
}
{
uint32_t w[4] =
{
op_(SpvOpConstant, 4),
m.ft,
id_cf1,
0
};
float one = 1.0f;
memcpy(&w[3], &one, sizeof(one));
sb_push_n(&te, w, 4);
}
{
uint32_t w[4] =
{
op_(SpvOpConstant, 4),
m.ft,
id_cl,
0
};
memcpy(&w[3], &lo, sizeof(lo));
sb_push_n(&te, w, 4);
}
{
uint32_t w[4] =
{
op_(SpvOpConstant, 4),
m.ft,
id_cr,
0
};
memcpy(&w[3], &ro, sizeof(ro));
sb_push_n(&te, w, 4);
}
{
uint32_t w[4] =
{
op_(SpvOpConstant, 4),
m.ft,
id_cc,
0
};
memcpy(&w[3], &conv, sizeof(conv));
sb_push_n(&te, w, 4);
}
{
uint32_t w[4] =
{
op_(SpvOpConstant, 4),
m.ft,
id_bg_expand,
0
};
float bg_expand = fmaxf(fabsf(lo * conv), fabsf(ro * conv));
memcpy(&w[3], &bg_expand, sizeof(bg_expand));
sb_push_n(&te, w, 4);
}
if (will_inj_vi)
{
uint32_t d[] =
{
op_(SpvOpDecorate, 4),
id_inj_view,
SpvDecorationBuiltIn,
SpvBuiltInViewIndex
};
sb_push_n(&ann, d, 4);
uint32_t v[] =
{
op_(SpvOpVariable, 4),
uint_,
id_inj_view,
SpvStorageInput
};
sb_push_n(&te, v, 4);
m.view_var = id_inj_view;
}
BodyCtx bc =
{
.m = &m,
.have_view = have_view,
.has_projection_path = false,
.has_view_path = false,
.uv4 = uv4,
.uint_ = uint_,
.bt = bt,
.cz = id_cz,
.cf0 = id_cf0,
.cf1 = id_cf1,
.cl = id_cl,
.cr = id_cr,
.cc = id_cc,
.projection_mode = projection_mode,
.force_far_depth = force_far_depth,
.procedural_sky = procedural_sky,
.bg_expand = id_bg_expand,
.lo_dbg = lo,
.ro_dbg = ro,
.dbg = dbg
};
size_t ins_ann = 0;
size_t ins_t = 0;
size_t ins_b = 0;
bool in_entry_function = false;
for (size_t i = 5; i < in_c;)
{
uint32_t opx = in[i] & 0xffff;
uint32_t wcx = in[i] >> 16;
if (!wcx || i + wcx > in_c)
break;
if (opx == SpvOpVariable && wcx >= 4)
{
}
if (opx == SpvOpTypeSampledImage && wcx >= 3)
{
}
if (!ins_ann &&
(opx == SpvOpTypeVoid ||
opx == SpvOpTypeBool ||
opx == SpvOpTypeInt ||
opx == SpvOpTypeFloat ||
opx == SpvOpTypeVector ||
opx == SpvOpTypeMatrix ||
opx == SpvOpTypeImage ||
opx == SpvOpTypeSampler ||
opx == SpvOpTypeSampledImage ||
opx == SpvOpTypeArray ||
opx == SpvOpTypeRuntimeArray ||
opx == SpvOpTypeStruct ||
opx == SpvOpTypeOpaque ||
opx == SpvOpTypePointer ||
opx == SpvOpTypeFunction ||
opx == SpvOpTypeForwardPointer ||
opx == SpvOpConstantTrue ||
opx == SpvOpConstantFalse ||
opx == SpvOpConstant ||
opx == SpvOpConstantComposite ||
opx == SpvOpVariable))
{
ins_ann = i;
}
if (opx == SpvOpFunction)
{
if (!ins_t)
ins_t = i;
in_entry_function =
(wcx >= 4 &&
in[i + 2] ==
(m.position_function ?
m.position_function :
m.entry_function));
}
if (in_entry_function &&
opx == SpvOpReturn)
{
ins_b = i;
}
if (in_entry_function &&
opx == SpvOpFunctionEnd)
{
break;
}
i += wcx;
}
if (!ins_t)
{
sb_free(&te);
free_spv_provenance(&m);
return false;
}
if (!is_gs && !ins_b)
{
sb_free(&te);
free_spv_provenance(&m);
return false;
}
bool need_mv_cap =
id_inj_view &&
!m.has_mv_cap;
bool mv_done = false;
bool ann_done = false;
bool te_done = false;
bool body_done = false;
SpvBuf ob;
if (!sb_init(&ob, in_c + te.n + 64))
{
sb_free(&te);
free_spv_provenance(&m);
return false;
}
uint32_t spv_version = in[1];
bool need_mv_ext =
need_mv_cap &&
((spv_version >> 16) == 1) &&
(((spv_version >> 8) & 0xff) == 0);
sb_push_n(&ob, in, 5);
bool ext_done = false;
for (size_t i = 5; i < in_c;)
{
if (!mv_done && need_mv_cap)
{
uint32_t c[] =
{
op_(SpvOpCapability, 2),
SpvCapabilityMultiView
};
sb_push_n(&ob, c, 2);
mv_done = true;
}
if (!ann_done && i == ins_ann)
{
sb_push_n(&ob, ann.w, ann.n);
ann_done = true;
}
if (!te_done && i == ins_t)
{
sb_push_n(&ob, te.w, te.n);
te_done = true;
}
uint32_t opx = in[i] & 0xffff;
uint32_t wcx = in[i] >> 16;
if (!wcx || i + wcx > in_c)
break;
if (!ext_done &&
need_mv_ext &&
opx != SpvOpCapability)
{
uint32_t e[] =
{
op_(SpvOpExtension, 6),
0x5F565053, 
0x5F52484B, 
0x746C756D, 
0x65697669, 
0x00000077 
};
sb_push_n(&ob, e, 6);
ext_done = true;
}
if (id_inj_view &&
opx == SpvOpEntryPoint &&
wcx >= 4 &&
(in[i + 1] == SpvExecVertex ||
in[i + 1] == SpvExecGeometry ||
in[i + 1] == SpvExecTessEval))
{
bool target_entry =
(wcx >= 3 &&
in[i + 2] == m.entry_function);
if (target_entry)
{
sb_push(
&ob,
((wcx + 1) << 16) |
SpvOpEntryPoint);
sb_push_n(
&ob,
&in[i + 1],
wcx - 1);
sb_push(
&ob,
id_inj_view);
}
else
{
sb_push_n(
&ob,
&in[i],
wcx);
}
i += wcx;
continue;
}
if (is_gs &&
opx == SpvOpEmitVertex)
{
emit_body(
&ob,
&bc,
&nid);
}
if (!is_gs &&
!body_done &&
i == ins_b)
{
emit_body(
&ob,
&bc,
&nid);
body_done = true;
}
if (opx == SpvOpConstantComposite && wcx >= 4)
{
bool has_spec_operand = false;
for (uint32_t operand = 3; operand < wcx && !has_spec_operand; ++operand)
{
uint32_t operand_id = in[i + operand];
for (size_t scan = 5; scan < in_c;)
{
uint32_t sw = in[scan] >> 16;
uint32_t sop = in[scan] & 0xffffu;
if (!sw || scan + sw > in_c)
break;
if ((sop == SpvOpSpecConstantTrue || sop == SpvOpSpecConstantFalse || sop == SpvOpSpecConstant || sop == SpvOpSpecConstantComposite || sop == SpvOpSpecConstantOp) && sw >= 3 && in[scan + 2] == operand_id)
{
has_spec_operand = true;
break;
}
scan += sw;
}
}
if (has_spec_operand)
{
sb_push(&ob, ((wcx << 16) | SpvOpSpecConstantComposite));
sb_push_n(&ob, &in[i + 1], wcx - 1);
i += wcx;
continue;
}
}
if (opx == SpvOpSelect &&
wcx >= 6 &&
id_v4bt &&
in[i + 1] == m.v4t)
{
uint32_t cond = in[i + 3];
uint32_t cond_type =
spirv_get_value_type(
in,
in_c,
cond);
if (cond_type == bt_for_types)
{
uint32_t vec_cond = nid++;
uint32_t cw[] =
{
op_(SpvOpCompositeConstruct, 7),
id_v4bt,
vec_cond,
cond,
cond,
cond,
cond
};
sb_push_n(&ob, cw, 7);
uint32_t sw[] =
{
op_(SpvOpSelect, 6),
in[i + 1],
in[i + 2],
vec_cond,
in[i + 4],
in[i + 5]
};
sb_push_n(&ob, sw, 6);
i += wcx;
continue;
}
}
if (opx == SpvOpStore &&
wcx >= 3)
{
uint32_t ptr_id = in[i + 1];
uint32_t obj_id = in[i + 2];
uint32_t ptr_type =
spirv_get_pointer_pointee_type(
in,
in_c,
ptr_id);
uint32_t obj_type =
spirv_get_value_type(
in,
in_c,
obj_id);
if (ptr_type &&
obj_type &&
ptr_type != obj_type)
{
if (obj_type == m.v4t &&
spirv_is_vector_type(
in,
in_c,
ptr_type,
m.ft,
3))
{
uint32_t repaired = nid++;
uint32_t rw[] =
{
op_(SpvOpVectorShuffle, 8),
ptr_type,
repaired,
obj_id,
obj_id,
0,
1,
2
};
sb_push_n(
&ob,
rw,
8);
uint32_t sw[] =
{
op_(SpvOpStore, 3),
ptr_id,
repaired
};
sb_push_n(
&ob,
sw,
3);
i += wcx;
continue;
}
}
}
sb_push_n(
&ob,
&in[i],
wcx);
i += wcx;
}
if (!ext_done && need_mv_ext)
{
uint32_t e[] =
{
op_(SpvOpExtension, 6),
0x5F565053,
0x5F52484B,
0x746C756D,
0x65697669,
0x00000077
};
sb_push_n(&ob, e, 6);
}
if (!ann_done)
sb_push_n(&ob, ann.w, ann.n);
if (!te_done)
sb_push_n(&ob, te.w, te.n);
for (size_t j = 5; j < ob.n;)
{
uint32_t opj = ob.w[j] & 0xffff;
uint32_t wcj = ob.w[j] >> 16;
if (opj == SpvOpVariable &&
wcj >= 4 &&
ob.w[j + 2] == id_inj_view)
{
}
if ((opj == SpvOpTypePointer ||
opj == SpvOpTypeInt ||
opj == SpvOpTypeVector) &&
wcj >= 2)
{
}
if (opj == SpvOpTypeSampledImage && wcj >= 3)
{
}
if (!wcj || j + wcj > ob.n)
break;
if (opj == SpvOpVariable && wcj >= 4)
{
}
if (opj == SpvOpTypePointer && wcj >= 4)
{
}
if (opj == SpvOpTypeInt && wcj >= 4)
{
}
if (opj == SpvOpVariable &&
wcj >= 4 &&
ob.w[j + 2] == id_inj_view)
{
}
j += wcj;
}
sb_free(&ann);
sb_free(&te);
ob.w[3] = nid;
for (size_t j = 5; j < ob.n;)
{
uint32_t op = ob.w[j] & 0xffff;
uint32_t wc = ob.w[j] >> 16;
if (op == SpvOpLoad && wc >= 4)
{
if (ob.w[j + 3] == m.view_var)
{
}
}
if (op == SpvOpStore && wc >= 3)
{
}
if (!wc || j + wc > ob.n)
break;
j += wc;
}
*out = ob.w;
*out_c = ob.n;
free_spv_provenance(&m);
return true;
}
void spirv_patched_free(uint32_t *w) { free(w); }
#define FS_MAX_IMG 64
#define FS_MAX_SI 64
#define FS_MAX_LOADS 512
#define FS_MAX_PARAMS 256
#define FS_MAX_CALLS 256
#define FS_MAX_FUNCTIONS 64
#define FS_MAX_VARS 128
typedef struct
{
uint32_t id; 
uint32_t source_id; 
uint32_t owner_var; 
uint32_t binding; 
bool from_projection;
bool from_view;
} FsLoadInfo;
typedef struct
{
uint32_t id;
uint32_t type;
uint32_t function_id;
uint32_t index;
} FsParameterInfo;
typedef struct
{
uint32_t id;
uint32_t first_param;
uint32_t type_id;
} FsFunctionInfo;
typedef struct
{
uint32_t function_id;
uint32_t parameter_index; 
uint32_t parameter_id; 
uint32_t argument_var; 
} FsCallInfo;
typedef struct
{
uint32_t id;
uint32_t type;
uint32_t storage;
uint32_t set;
uint32_t binding;
uint32_t location;
bool is_projection_ubo;
} FsVariableInfo;
typedef struct
{
uint32_t target;
uint32_t binding;
uint32_t set;
uint32_t location;
} FsDecorationInfo;
typedef struct
{
uint32_t id;
uint32_t sampled_type_id;
uint32_t sampled_type;
uint32_t dim;
uint32_t depth;
uint32_t arrayed;
uint32_t ms;
uint32_t sampled;
uint32_t format;
bool patchable;
uint32_t pointer_type;
uint32_t owner_var;
uint32_t binding;
uint32_t set;
bool stereo;
uint32_t replacement_type; 
uint32_t replacement_pointer_type;
uint32_t replacement_sampled_type;
} FsImageInfo;
typedef struct
{
FsImageInfo images[FS_MAX_IMG];
uint32_t n_img;
uint32_t si_ids[FS_MAX_SI];
uint32_t n_si;
FsLoadInfo loads[FS_MAX_LOADS];
uint32_t n_load;
FsParameterInfo params[FS_MAX_PARAMS];
uint32_t n_param;
FsCallInfo calls[FS_MAX_CALLS];
uint32_t n_call;
FsVariableInfo vars[FS_MAX_VARS];
uint32_t n_var;
FsDecorationInfo decorations[FS_MAX_VARS];
uint32_t n_dec;
uint32_t float_id;
uint32_t int_id;
uint32_t uint_id;
uint32_t v2int_id;
uint32_t v2uint_id;
uint32_t v3int_id;
uint32_t v3uint_id;
uint32_t v3float_id;
uint32_t ptr_int_in_id;
uint32_t vi_var_id;
bool has_mv_cap;
size_t ep_word;
size_t fn_word;
FsFunctionInfo functions[FS_MAX_FUNCTIONS];
uint32_t n_function;
bool in_function;
uint32_t current_function_id;
uint32_t current_param_index;
} FsScan;
static bool
fs_id_in(
const uint32_t *arr,
uint32_t n,
uint32_t id)
{
for (uint32_t i = 0; i < n; i++)
{
if (arr[i] == id)
return true;
}
return false;
}
static int
fs_var_index(
const FsScan *s,
uint32_t id)
{
if (!s)
return -1;
for (uint32_t i = 0; i < s->n_var; ++i)
{
if (s->vars[i].id == id)
return (int)i;
}
return -1;
}
static void
fs_dump_descriptor_chain(
const FsScan *s,
const uint32_t *spv,
size_t word_count,
uint32_t descriptor_var)
{
int vi = fs_var_index(s, descriptor_var);
if (vi < 0)
return;
uint32_t type_id = s->vars[vi].type;
for (size_t i = 5; i < word_count; )
{
uint32_t wc = spv[i] >> 16;
uint32_t op = spv[i] & 0xffff;
if (!wc || i + wc > word_count)
break;
if ((op == SpvOpTypePointer ||
op == SpvOpTypeSampledImage ||
op == SpvOpTypeImage) &&
spv[i + 1] == type_id)
{
if (op == SpvOpTypePointer && wc >= 4)
{
type_id = spv[i + 3];
i = 5;
continue;
}
if (op == SpvOpTypeSampledImage && wc >= 3)
{
type_id = spv[i + 2];
i = 5;
continue;
}
if (op == SpvOpTypeImage && wc >= 9)
{
break;
}
}
i += wc;
}
}
static int
fs_dec_index(
const FsScan *s,
uint32_t target)
{
if (!s)
return -1;
for (uint32_t i = 0; i < s->n_dec; ++i)
{
if (s->decorations[i].target == target)
return (int)i;
}
return -1;
}
static bool
fs_binding_is_stereo_attachment(
const FsScan *s,
uint32_t var)
{
int vi = fs_var_index(s, var);
if (vi < 0)
{
return false;
}
const FsVariableInfo *v = &s->vars[vi];
if (v->storage == SpvStorageClassInput)
{
return true;
}
bool stereo =
(v->binding <= 4);
return stereo;
}
static uint32_t fs_result_type_of(FsScan *s,
const uint32_t *in,
size_t in_c,
uint32_t result_id)
{
for (size_t i = 5; i < in_c;)
{
uint32_t wc = in[i] >> 16;
uint32_t op = in[i] & 0xffff;
if (!wc)
break;
if ((op == SpvOpCompositeConstruct ||
op == SpvOpConstant ||
op == SpvOpConstantComposite ||
op == SpvOpCompositeExtract ||
op == SpvOpVectorShuffle ||
op == SpvOpLoad ||
op == SpvOpAccessChain ||
op == SpvOpCopyObject ||
op == SpvOpBitcast ||
op == SpvOpPhi ||
op == SpvOpImageFetch) &&
wc >= 3 &&
in[i + 2] == result_id)
{
return in[i + 1];
}
i += wc;
}
return 0;
}
static bool
fs_should_patch_sample(
const FsScan *s,
uint64_t spv_hash,
uint32_t descriptor_var)
{
int vi = fs_var_index(s, descriptor_var);
if (vi < 0)
return false;
uint32_t binding = s->vars[vi].binding;
uint32_t set = s->vars[vi].set;
//
return fs_binding_is_stereo_attachment(s, descriptor_var);
}
static const char *
spv_op_name(uint32_t op)
{
switch (op)
{
case SpvOpCopyObject:
return "OpCopyObject";
case SpvOpVariable:
return "OpVariable";
case SpvOpLoad:
return "OpLoad";
case SpvOpSampledImage:
return "OpSampledImage";
case SpvOpImage:
return "OpImage";
case SpvOpImageSampleImplicitLod:
return "OpImageSampleImplicitLod";
case SpvOpImageQuerySize:
return "OpImageQuerySize";
case SpvOpImageQuerySizeLod:
return "OpImageQuerySizeLod";
case SpvOpImageSampleExplicitLod:
return "OpImageSampleExplicitLod";
case SpvOpImageSampleDrefImplicitLod:
return "OpImageSampleDrefImplicitLod";
case SpvOpImageSampleDrefExplicitLod:
return "OpImageSampleDrefExplicitLod";
case SpvOpImageGather:
return "OpImageGather";
case SpvOpImageDrefGather:
return "OpImageDrefGather";
case SpvOpImageFetch:
return "OpImageFetch";
case SpvOpImageRead:
return "OpImageRead";
case SpvOpImageWrite:
return "OpImageWrite";
case SpvOpImageSparseSampleImplicitLod:
return "OpImageSparseSampleImplicitLod";
case SpvOpImageSparseSampleExplicitLod:
return "OpImageSparseSampleExplicitLod";
case SpvOpImageSparseFetch:
return "OpImageSparseFetch";
case SpvOpImageSparseRead:
return "OpImageSparseRead";
case SpvOpImageSparseTexelsResident:
return "OpImageSparseTexelsResident";
case SpvOpFunctionParameter:
return "OpFunctionParameter";
default:
return "Unknown";
}
}
static bool
fs_is_image_related_type(
const FsScan *s,
uint32_t type)
{
if (!s)
return false;
for (uint32_t i = 0; i < s->n_img; ++i)
{
if (s->images[i].id == type)
return true;
}
for (uint32_t i = 0; i < s->n_si; ++i)
{
if (s->si_ids[i] == type)
return true;
}
return false;
}
static int
fs_find_function(
const FsScan *s,
uint32_t function_id)
{
if (!s)
return -1;
for (uint32_t i = 0; i < s->n_function; ++i)
{
if (s->functions[i].id == function_id)
return (int)i;
}
return -1;
}
static int
fs_find_load(
const FsScan *s,
uint32_t value_id)
{
if (!s)
{
return -1;
}
for (uint32_t i = 0; i < s->n_load; ++i)
{
if (s->loads[i].id == value_id)
{
return (int)i;
}
}
return -1;
}
static int
fs_find_parameter(
const FsScan *s,
uint32_t parameter_id)
{
if (!s)
return -1;
for (uint32_t i = 0; i < s->n_param; ++i)
{
if (s->params[i].id == parameter_id)
return (int)i;
}
return -1;
}
static int
fs_find_call_parameter(
const FsScan *s,
uint32_t parameter_id)
{
if (!s)
return -1;
for (uint32_t i = 0; i < s->n_call; ++i)
{
if (s->calls[i].parameter_id == parameter_id)
return (int)i;
}
return -1;
}
static void
fs_add_load_mapping(
FsScan *s,
uint32_t value_id,
uint32_t owner)
{
if (!s)
return;
int index =
fs_find_load(
s,
value_id);
if (index >= 0)
{
s->loads[index].owner_var = owner;
return;
}
if (s->n_load >= FS_MAX_LOADS)
{
return;
}
FsLoadInfo *load =
&s->loads[s->n_load++];
memset(
load,
0,
sizeof(*load));
load->id =
value_id;
load->owner_var =
owner;
load->source_id =
value_id;
load->binding =
0xffffffffu;
}
static bool
fs_resolve_load_owner(
const FsScan *s,
uint32_t value_id,
uint32_t *owner)
{
if (!s)
return false;
int index =
fs_find_load(
s,
value_id);
if (index < 0)
{
return false;
}
if (owner)
{
*owner =
s->loads[index].owner_var;
}
return true;
}
static uint32_t
fs_find_matching_sampled_image(
const uint32_t *in,
size_t in_c,
uint32_t image_type)
{
for (size_t i = 5; i < in_c;)
{
uint32_t wc = in[i] >> 16;
uint32_t op = in[i] & 0xffff;
if (!wc || i + wc > in_c)
break;
if (op == SpvOpTypeSampledImage &&
wc >= 3 &&
in[i + 2] == image_type)
{
return in[i + 1];
}
i += wc;
}
return 0;
}
static uint32_t
fs_find_matching_image_type(
const uint32_t *in,
size_t in_c,
uint32_t sampled_type,
uint32_t dim,
uint32_t depth,
uint32_t arrayed,
uint32_t ms,
uint32_t sampled,
uint32_t format)
{
for (size_t i = 5; i < in_c;)
{
uint32_t wc = in[i] >> 16;
uint32_t op = in[i] & 0xffff;
if (!wc || i + wc > in_c)
break;
if (op == SpvOpTypeImage &&
wc >= 9 &&
in[i + 2] == sampled_type &&
in[i + 3] == dim &&
in[i + 4] == depth &&
in[i + 5] == arrayed &&
in[i + 6] == ms &&
in[i + 7] == sampled &&
in[i + 8] == format)
{
return in[i + 1];
}
i += wc;
}
return 0;
}
static int
fs_find_matching_array_image(FsScan *s, const FsImageInfo *src)
{
for (uint32_t i = 0; i < s->n_img; ++i)
{
const FsImageInfo *img = &s->images[i];
if (img->id == src->id) continue;
if (img->dim != src->dim) continue;
if (img->depth != src->depth) continue;
if (img->ms != src->ms) continue;
if (img->sampled != src->sampled) continue;
if (img->format != src->format) continue;
if (img->arrayed == 1)
{
return (int)i;
}
}
return -1;
}
static int
fs_find_image_by_sampled_image(
FsScan *s,
uint32_t sampled_image_type)
{
for (uint32_t i = 0; i < s->n_img; ++i)
{
if (s->images[i].sampled_type_id == sampled_image_type)
return (int)i;
}
return -1;
}
static int
fs_find_image_by_owner(
FsScan *s,
uint32_t owner_var)
{
for (uint32_t i = 0; i < s->n_img; ++i)
{
if (s->images[i].owner_var == owner_var)
return (int)i;
}
return -1;
}
static void
fs_scan_type_instruction(
FsScan *s,
const uint32_t *ins,
uint32_t op,
uint32_t wc)
{
if (!s || !ins)
return;
switch (op)
{
case SpvOpTypeFloat:
if (wc >= 3 && ins[2] == 32)
{
s->float_id = ins[1];
}
break;
case SpvOpTypeInt:
if (wc >= 4 &&
ins[2] == 32)
{
if (ins[3] == 1)
s->int_id = ins[1];
else
s->uint_id = ins[1];
}
break;
case SpvOpTypeVector:
if (wc >= 4)
{
if (ins[2] == s->int_id)
{
if (ins[3] == 2)
s->v2int_id = ins[1];
else if (ins[3] == 3)
s->v3int_id = ins[1];
}
else if (ins[2] == s->uint_id)
{
if (ins[3] == 2)
s->v2uint_id = ins[1];
else if (ins[3] == 3)
s->v3uint_id = ins[1];
}
else if (ins[2] == s->float_id &&
ins[3] == 3)
{
s->v3float_id = ins[1];
}
}
break;
case SpvOpTypeImage:
{
if (wc < 9)
break;
uint32_t type_id = ins[1];
uint32_t sampled_type = ins[2];
uint32_t dim = ins[3];
uint32_t depth = ins[4];
uint32_t arrayed = ins[5];
uint32_t ms = ins[6];
uint32_t sampled = ins[7];
uint32_t format = ins[8];
if (dim == SpvDim2D &&
s->n_img < FS_MAX_IMG)
{
FsImageInfo *img =
&s->images[s->n_img++];
memset(img, 0, sizeof(*img));
img->id = type_id;
img->sampled_type = sampled_type;
img->dim = dim;
img->depth = depth;
img->arrayed = arrayed;
img->ms = ms;
img->sampled = sampled;
img->format = format;
img->patchable = (arrayed == 0);
img->stereo = (arrayed == 0);
img->replacement_type = 0;
}
break;
}
case SpvOpTypeSampledImage:
{
if (wc < 3)
break;
uint32_t sampled_image_id = ins[1];
uint32_t image_type_id = ins[2];
for (uint32_t ii = 0; ii < s->n_img; ++ii)
{
if (s->images[ii].id == image_type_id)
{
s->images[ii].sampled_type_id = sampled_image_id;
break;
}
}
if (s->n_si < FS_MAX_SI)
s->si_ids[s->n_si++] = sampled_image_id;
break;
}
case SpvOpTypePointer:
for (uint32_t img = 0; img < s->n_img; ++img)
{
}
if (wc >= 4 &&
ins[2] == SpvStorageClassInput &&
s->int_id &&
ins[3] == s->int_id)
{
s->ptr_int_in_id = ins[1];
}
if (wc >= 4)
{
for (uint32_t img = 0; img < s->n_img; ++img)
{
if (s->images[img].sampled_type_id == ins[3])
{
s->images[img].pointer_type = ins[1];
break;
}
}
}
break;
default:
break;
}
}
static void
fs_process_decoration(
FsScan *s,
const uint32_t *ins,
uint32_t wc)
{
if (!s || !ins || wc < 4)
return;
uint32_t target = ins[1];
uint32_t decoration = ins[2];
uint32_t value = ins[3];
if (target == 15)
{
}
if (decoration == SpvDecorationBuiltIn &&
value == SpvBuiltInViewIndex)
{
s->vi_var_id = target;
return;
}
if (decoration == SpvDecorationLocation)
{
int index = fs_var_index(s, target);
if (index >= 0)
{
s->vars[index].location = value;
}
return;
}
if (decoration != SpvDecorationBinding &&
decoration != SpvDecorationDescriptorSet)
{
return;
}
int index = -1;
for (uint32_t i = 0; i < s->n_dec; ++i)
{
if (s->decorations[i].target == target)
{
index = (int)i;
break;
}
}
if (index < 0)
{
if (s->n_dec >= FS_MAX_VARS)
{
return;
}
index = (int)s->n_dec++;
FsDecorationInfo *dec = &s->decorations[index];
memset(dec, 0, sizeof(*dec));
dec->target = target;
dec->set = 0xffffffffu;
dec->binding = 0xffffffffu;
dec->location = 0xffffffffu;
}
FsDecorationInfo *dec = &s->decorations[index];
if (decoration == SpvDecorationBinding)
dec->binding = value;
else
dec->set = value;
int var_index = fs_var_index(s, target);
if (var_index >= 0)
{
FsVariableInfo *var = &s->vars[var_index];
if (decoration == SpvDecorationBinding)
var->binding = value;
else
var->set = value;
if (var->storage == SpvStorageClassUniform &&
var->binding == 4u)
{
var->is_projection_ubo = true;
}
}
}
static void
fs_scan_variable_instruction(
FsScan *s,
const uint32_t *ins,
uint32_t wc)
{
if (!s || !ins || wc < 4)
return;
if (s->n_var >= FS_MAX_VARS)
{
return;
}
FsVariableInfo *var = &s->vars[s->n_var++];
memset(var, 0, sizeof(*var));
var->id = ins[2];
var->type = ins[1];
var->storage = ins[3];
var->set = 0xffffffffu;
var->binding = 0xffffffffu;
var->location = 0xffffffffu;
var->is_projection_ubo = false;
for (uint32_t i = 0; i < s->n_dec; ++i)
{
FsDecorationInfo *dec = &s->decorations[i];
if (dec->target != var->id)
continue;
if (dec->set != 0xffffffffu)
var->set = dec->set;
if (dec->binding != 0xffffffffu)
var->binding = dec->binding;
if (dec->location != 0xffffffffu)
var->location = dec->location;
}
for (uint32_t ii = 0; ii < s->n_img; ++ii)
{
}
if (var->storage == SpvStorageClassUniformConstant)
{
for (uint32_t ii = 0; ii < s->n_img; ++ii)
{
if (s->images[ii].id == var->type)
{
break;
}
}
}
if (var->storage == SpvStorageClassUniform &&
var->binding == 4u)
{
var->is_projection_ubo = true;
}
if (var->id == 15)
{
}
if (var->storage == SpvStorageClassUniform ||
var->storage == SpvStorageClassUniformConstant)
{
}
}
static void
fs_scan_function_parameter(
FsScan *s,
const uint32_t *ins,
uint32_t wc)
{
if (wc < 3)
return;
if (!s->in_function)
{
return;
}
if (s->n_param >= FS_MAX_PARAMS)
{
return;
}
uint32_t idx =
s->n_param++;
s->params[idx].id =
ins[2];
s->params[idx].type =
ins[1];
s->params[idx].function_id =
s->current_function_id;
s->params[idx].index =
s->current_param_index;
s->current_param_index++;
}
static void
fs_scan_function(
FsScan *s,
const uint32_t *ins,
uint32_t wc)
{
if (!s || !ins || wc < 3)
return;
uint32_t function_id =
ins[2];
s->in_function = true;
s->current_function_id = function_id;
s->current_param_index = 0;
if (s->n_function >= FS_MAX_FUNCTIONS)
{
return;
}
FsFunctionInfo *fn =
&s->functions[s->n_function++];
fn->id =
function_id;
fn->first_param =
s->n_param;
fn->type_id =
(wc >= 5) ? ins[4] : 0;
}
static void
fs_scan_load_instruction(
FsScan *s,
const uint32_t *ins,
uint32_t wc)
{
if (!s || !ins || wc < 4)
return;
uint32_t result_type = ins[1];
uint32_t result_id = ins[2];
uint32_t source_id = ins[3];
uint32_t owner = 0;
bool have_owner = fs_resolve_load_owner(s, source_id, &owner);
if (!have_owner)
{
owner = source_id;
}
int owner_var_index = fs_var_index(s, owner);
bool from_projection_ubo =
(owner_var_index >= 0 &&
s->vars[owner_var_index].is_projection_ubo);
bool image_related =
fs_is_image_related_type(s, result_type);
if (!image_related && !from_projection_ubo)
return;
if (s->n_load >= FS_MAX_LOADS)
{
return;
}
FsLoadInfo *li = &s->loads[s->n_load++];
memset(li, 0, sizeof(*li));
li->id = result_id;
li->source_id = source_id;
li->owner_var = owner;
li->binding = 0xffffffffu;
li->from_projection = from_projection_ubo;
li->from_view = false;
if (owner_var_index >= 0)
{
li->binding = s->vars[owner_var_index].binding;
}
int src_var = fs_var_index(s, source_id);
if (src_var >= 0)
{
}
else
{
}
if (from_projection_ubo)
{
}
}
static void
fs_scan_function_call(
FsScan *s,
const uint32_t *ins,
uint32_t wc)
{
if (!s || !ins || wc < 4)
return;
uint32_t result_id =
ins[2];
uint32_t function_id =
ins[3];
uint32_t argument_count =
wc - 4;
for (uint32_t arg = 0;
arg < argument_count;
++arg)
{
uint32_t value =
ins[4 + arg];
if (s->n_call >= FS_MAX_CALLS)
{
continue;
}
FsCallInfo *call =
&s->calls[s->n_call++];
memset(
call,
0,
sizeof(*call));
call->function_id =
function_id;
call->parameter_index =
arg;
call->argument_var =
value;
call->parameter_id =
0;
}
}
static void
fs_track_image_propagation(
FsScan *s,
const uint32_t *ins,
uint32_t op,
uint32_t wc)
{
if (!s || wc < 4)
return;
bool propagate =
(op == SpvOpImage) ||
(op == SpvOpCopyObject);
if (!propagate)
return;
uint32_t result_id = ins[2];
uint32_t source_id = ins[3];
int src =
fs_find_load(
s,
source_id);
if (src < 0)
{
return;
}
if (s->n_load >= FS_MAX_LOADS)
{
return;
}
FsLoadInfo *dst =
&s->loads[s->n_load++];
*dst = s->loads[src];
dst->id = result_id;
}
static void
fs_track_sampled_image(
FsScan *s,
const uint32_t *ins,
uint32_t wc)
{
if (!s || wc < 5)
return;
if (!fs_id_in(
s->si_ids,
s->n_si,
ins[1]))
{
return;
}
uint32_t result_id = ins[2];
uint32_t image_id = ins[3];
uint32_t sampler_id = ins[4];
int src =
fs_find_load(
s,
image_id);
if (src < 0)
{
return;
}
if (s->n_load >= FS_MAX_LOADS)
{
return;
}
FsLoadInfo *dst =
&s->loads[s->n_load++];
*dst = s->loads[src];
dst->id = result_id;
}
static void
fs_scan_image_operation(
FsScan *s,
const uint32_t *ins,
uint32_t op,
uint32_t wc)
{
if (!s || wc < 5)
return;
switch (op)
{
case SpvOpImageSampleImplicitLod:
case SpvOpImageSampleExplicitLod:
case SpvOpImageSampleDrefImplicitLod:
case SpvOpImageSampleDrefExplicitLod:
case SpvOpImageFetch:
case SpvOpImageRead:
case SpvOpImageWrite:
case SpvOpImageTexelPointer:
break;
default:
return;
}
uint32_t image_id = ins[3];
int load =
fs_find_load(
s,
image_id);
if (load < 0)
{
return;
}
FsLoadInfo *li =
&s->loads[load];
if (li->owner_var == 0)
{
return;
}
int var =
fs_var_index(
s,
li->owner_var);
if (var < 0)
{
return;
}
bool stereo =
fs_binding_is_stereo_attachment(
s,
li->owner_var);
li->binding =
s->vars[var].binding;
if (stereo)
{
}
}
static void
fs_scan_instruction(
FsScan *s,
const uint32_t *ins,
uint32_t op,
uint32_t wc)
{
if (!s || !ins)
return;
switch (op)
{
case SpvOpImageSampleImplicitLod:
case SpvOpImageSampleExplicitLod:
case SpvOpImageSampleDrefImplicitLod:
case SpvOpImageSampleDrefExplicitLod:
case SpvOpImageFetch:
case SpvOpImageRead:
case SpvOpImageWrite:
case SpvOpImageQuerySizeLod:
break;
default:
break;
}
switch (op)
{
case SpvOpTypeFloat:
case SpvOpTypeInt:
case SpvOpTypeVector:
case SpvOpTypeImage:
case SpvOpTypeSampledImage:
case SpvOpTypePointer:
fs_scan_type_instruction(
s,
ins,
op,
wc);
break;
case SpvOpDecorate:
fs_process_decoration(
s,
ins,
wc);
break;
case SpvOpVariable:
fs_scan_variable_instruction(
s,
ins,
wc);
break;
case SpvOpFunction:
fs_scan_function(
s,
ins,
wc);
break;
case SpvOpFunctionParameter:
fs_scan_function_parameter(
s,
ins,
wc);
break;
case SpvOpFunctionEnd:
s->in_function = false;
s->current_function_id = 0;
s->current_param_index = 0;
break;
case SpvOpFunctionCall:
fs_scan_function_call(
s,
ins,
wc);
break;
case SpvOpAccessChain:
case SpvOpInBoundsAccessChain:
case SpvOpPtrAccessChain:
{
if (wc >= 4)
{
uint32_t result_id = ins[2];
uint32_t base_id = ins[3];
uint32_t owner = base_id;
if (!fs_resolve_load_owner(s, base_id, &owner))
{
if (fs_var_index(s, base_id) >= 0)
owner = base_id;
}
fs_add_load_mapping(s, result_id, owner);
}
break;
}
case SpvOpCopyObject:
case SpvOpBitcast:
{
if (wc >= 4)
{
uint32_t result_id = ins[2];
uint32_t source_id = ins[3];
uint32_t owner = source_id;
if (!fs_resolve_load_owner(s, source_id, &owner))
{
if (fs_var_index(s, source_id) >= 0)
owner = source_id;
}
fs_add_load_mapping(s, result_id, owner);
}
break;
}
case SpvOpCompositeExtract:
{
if (wc >= 5)
{
uint32_t result_id = ins[2];
uint32_t source_id = ins[3];
uint32_t owner = source_id;
if (!fs_resolve_load_owner(s, source_id, &owner))
{
if (fs_var_index(s, source_id) >= 0)
owner = source_id;
}
fs_add_load_mapping(s, result_id, owner);
}
break;
}
case SpvOpVectorShuffle:
{
if (wc >= 6)
{
uint32_t result_id = ins[2];
uint32_t source_id = ins[3];
uint32_t owner = source_id;
if (!fs_resolve_load_owner(s, source_id, &owner))
{
if (fs_var_index(s, source_id) >= 0)
owner = source_id;
}
fs_add_load_mapping(s, result_id, owner);
}
break;
}
case SpvOpLoad:
{
if (wc >= 4)
{
}
fs_scan_load_instruction(
s,
ins,
wc);
break;
}
case SpvOpSampledImage:
{
if (wc >= 5)
{
int src = fs_find_load(s, ins[3]);
}
fs_track_sampled_image(
s,
ins,
wc);
break;
}
case SpvOpImage:
fs_track_image_propagation(
s,
ins,
op,
wc);
break;
case SpvOpImageSampleDrefExplicitLod:
case SpvOpImageFetch:
case SpvOpImageRead:
case SpvOpImageWrite:
case SpvOpImageTexelPointer:
case SpvOpImageQuerySizeLod:
fs_scan_image_operation(
s,
ins,
op,
wc);
break;
default:
break;
}
}
static void
fs_fixup_function_parameters(
FsScan *s);
static void
fs_dump_scan_summary(
const FsScan *s);
static void
fs_prescan(
FsScan *s,
const uint32_t *w,
size_t c)
{
if (!s || !w || c < 5)
{
return;
}
memset(
s,
0,
sizeof(*s));
for (size_t i = 5; i < c;)
{
uint32_t word =
w[i];
uint32_t op =
word & 0xffffu;
uint32_t wc =
word >> 16;
if (wc == 0 ||
i + wc > c)
{
break;
}
fs_scan_instruction(
s,
&w[i],
op,
wc);
i += wc;
}
fs_fixup_function_parameters(
s);
for (uint32_t p = 0; p < s->n_param; ++p)
{
}
for (uint32_t cidx = 0; cidx < s->n_call; ++cidx)
{
}
for (uint32_t l = 0; l < s->n_load; ++l)
{
FsLoadInfo *load =
&s->loads[l];
for (uint32_t cidx = 0;
cidx < s->n_call;
++cidx)
{
FsCallInfo *call =
&s->calls[cidx];
if (load->owner_var ==
call->parameter_id)
{
load->owner_var =
call->argument_var;
break;
}
}
}
fs_dump_scan_summary(s);
FsImageInfo original_images[FS_MAX_IMG];
uint32_t original_count = s->n_img;
memcpy(original_images, s->images,
original_count * sizeof(FsImageInfo));
s->n_img = 0;
for (uint32_t img = 0; img < original_count; ++img)
{
const FsImageInfo *src = &original_images[img];
bool found = false;
for (uint32_t v = 0; v < s->n_var; ++v)
{
if (src->pointer_type &&
s->vars[v].type == src->pointer_type)
{
if (s->n_img >= FS_MAX_IMG)
break;
FsImageInfo *dst = &s->images[s->n_img++];
*dst = *src;
dst->replacement_type = 0;
dst->replacement_pointer_type = 0;
dst->replacement_sampled_type = 0;
dst->owner_var = s->vars[v].id;
dst->binding = s->vars[v].binding;
dst->set = s->vars[v].set;
dst->stereo =
fs_binding_is_stereo_attachment(
s,
dst->owner_var);
found = true;
}
}
if (!found)
{
if (s->n_img >= FS_MAX_IMG)
break;
FsImageInfo *dst = &s->images[s->n_img++];
*dst = *src;
dst->replacement_type = 0;
dst->replacement_pointer_type = 0;
dst->replacement_sampled_type = 0;
dst->owner_var = UINT32_MAX;
dst->binding = UINT32_MAX;
dst->set = UINT32_MAX;
}
}
for (uint32_t l = 0; l < s->n_load; ++l)
{
const FsLoadInfo *load = &s->loads[l];
int vi = fs_var_index(s, load->owner_var);
}
for (uint32_t v = 0; v < s->n_var; ++v)
{
if (s->vars[v].id == 15)
{
}
}
for (uint32_t i = 0; i < s->n_img; ++i)
{
}
}
static bool
fs_prescan_module(
FsScan *s,
const uint32_t *w,
size_t c)
{
if (!s || !w || c < 5)
{
return false;
}
fs_prescan(
s,
w,
c);
if (s->n_var == 0 &&
s->n_img == 0 &&
s->n_load == 0)
{
}
return true;
}
static void
fs_fixup_function_parameters(
FsScan *s)
{
if (!s)
return;
for (uint32_t i = 0; i < s->n_call; ++i)
{
FsCallInfo *call =
&s->calls[i];
int fn =
fs_find_function(
s,
call->function_id);
if (fn < 0)
continue;
FsFunctionInfo *func =
&s->functions[fn];
uint32_t param_index =
func->first_param +
call->parameter_index;
if (param_index >= s->n_param)
continue;
call->parameter_id =
s->params[param_index].id;
}
for (uint32_t i = 0; i < s->n_load; ++i)
{
FsLoadInfo *load =
&s->loads[i];
int p =
fs_find_parameter(
s,
load->owner_var);
if (p < 0)
continue;
FsParameterInfo *param =
&s->params[p];
bool resolved = false;
for (uint32_t c = 0; c < s->n_call; ++c)
{
FsCallInfo *call =
&s->calls[c];
if (call->function_id !=
param->function_id)
continue;
if (call->parameter_id !=
param->id)
continue;
load->owner_var =
call->argument_var;
resolved = true;
break;
}
if (!resolved)
{
}
}
uint32_t unresolved = 0;
for (uint32_t i = 0; i < s->n_load; ++i)
{
int var =
fs_var_index(
s,
s->loads[i].owner_var);
if (var < 0)
++unresolved;
}
}
static void
fs_dump_scan_summary(
const FsScan *s)
{
if (!s)
return;
for (uint32_t i = 0; i < s->n_img; ++i)
{
const FsImageInfo *img =
&s->images[i];
}
for (uint32_t i = 0; i < s->n_var; ++i)
{
const FsVariableInfo *v =
&s->vars[i];
}
for (uint32_t i = 0; i < s->n_dec; ++i)
{
const FsDecorationInfo *d =
&s->decorations[i];
}
for (uint32_t i = 0; i < s->n_function; ++i)
{
const FsFunctionInfo *fn =
&s->functions[i];
}
for (uint32_t i = 0; i < s->n_param; ++i)
{
const FsParameterInfo *p =
&s->params[i];
}
for (uint32_t i = 0; i < s->n_load; ++i)
{
const FsLoadInfo *load =
&s->loads[i];
int var =
fs_var_index(
s,
load->owner_var);
if (var >= 0)
{
}
else
{
}
}
for (uint32_t i = 0; i < s->n_call; ++i)
{
const FsCallInfo *call =
&s->calls[i];
}
}
static uint32_t
fs_count_patches(
const FsScan *s,
const uint32_t *w,
size_t c)
{
uint32_t count = 0;
bool in_func = false;
for (size_t i = 5; i < c;)
{
uint32_t op = w[i] & 0xffffu;
uint32_t wc = w[i] >> 16;
if (!wc || i + wc > c)
break;
if (op == SpvOpFunction)
in_func = true;
if (in_func &&
wc >= 5 &&
(op == SpvOpImageSampleImplicitLod ||
op == SpvOpImageSampleExplicitLod ||
op == SpvOpImageSampleDrefImplicitLod ||
op == SpvOpImageSampleDrefExplicitLod))
{
if (fs_find_load(s, w[i + 3]) >= 0)
{
++count;
}
}
if (in_func &&
op == SpvOpImageFetch &&
wc >= 5)
{
uint32_t descriptor_var = 0;
int load =
fs_find_load(
s,
w[i + 3]);
if (load >= 0)
descriptor_var =
s->loads[load].owner_var;
if (descriptor_var == 0)
{
}
if (fs_should_patch_sample(s, hash_spv(w, c), descriptor_var))
{
uint32_t binding = 0xffffffffu;
int var =
fs_var_index(
s,
descriptor_var);
if (var >= 0)
binding =
s->vars[var].binding;
++count;
}
}
i += wc;
}
return count;
}
static int
fs_image_index(
const FsScan *s,
uint32_t id)
{
if (!s)
return -1;
for (uint32_t i = 0; i < s->n_img; ++i)
{
if (s->images[i].id == id)
return (int)i;
}
return -1;
}
static bool
fs_type_is_input_attachment(
const FsScan *s,
uint32_t type)
{
if (!s)
return false;
for (uint32_t i = 0; i < s->n_var; ++i)
{
if (s->vars[i].type == type &&
s->vars[i].storage ==
SpvStorageClassInput)
{
return true;
}
}
return false;
}
bool spirv_patch_stereo_fs(
const uint32_t *in, size_t in_c,
uint32_t **out, size_t *out_c)
{
if (!in || in_c < 5 || in[0] != SPIRV_MAGIC) return false;
uint64_t h = hash_spv(in, in_c);
FsScan s;
fs_prescan(&s, in, in_c);
if (s.n_img == 0 || !s.float_id)
{
return false;
}
uint32_t n_patches = fs_count_patches(&s, in, in_c);
uint32_t nid = in[3];
uint32_t new_int_id = s.int_id ? s.int_id : nid++;
uint32_t new_v3f_id = s.v3float_id ? s.v3float_id : nid++;
uint32_t new_v3i_id = s.v3int_id ? s.v3int_id : nid++;
uint32_t new_v3u_id = 0;
if (s.uint_id)
new_v3u_id = s.v3uint_id ? s.v3uint_id : nid++;
uint32_t new_pin_id = s.ptr_int_in_id ? s.ptr_int_in_id : nid++;
uint32_t new_vi_id = s.vi_var_id ? s.vi_var_id : nid++;
uint32_t new_vi_type = s.int_id ? s.int_id : new_int_id;
bool is_new_vi = (s.vi_var_id == 0);
bool emit_vi_decorate = is_new_vi;
bool emit_vi_variable = is_new_vi;
for (uint32_t img = 0; img < s.n_img; ++img)
{
if (!s.images[img].patchable)
continue;
uint32_t replacement = 0;
uint32_t replacement_sampled = 0;
for (uint32_t prev = 0; prev < img; ++prev)
{
if (!s.images[prev].patchable)
continue;
if (s.images[prev].sampled_type_id != s.images[img].sampled_type_id)
continue;
if (s.images[prev].dim != s.images[img].dim ||
s.images[prev].depth != s.images[img].depth ||
s.images[prev].arrayed != s.images[img].arrayed ||
s.images[prev].ms != s.images[img].ms ||
s.images[prev].sampled != s.images[img].sampled ||
s.images[prev].format != s.images[img].format)
continue;
if (!s.images[prev].replacement_type)
continue;
if (s.images[img].sampled != 2 &&
!s.images[prev].replacement_sampled_type)
continue;
replacement = s.images[prev].replacement_type;
replacement_sampled = s.images[prev].replacement_sampled_type;
break;
}
if (replacement == 0)
{
replacement = nid++;
bool sampled_type_reusable = s.images[img].sampled != 2 && s.images[img].sampled_type_id != 0;
for (uint32_t other = 0; other < s.n_img; ++other)
{
if (other == img || !s.images[other].patchable || s.images[other].sampled_type_id != s.images[img].sampled_type_id)
continue;
if (s.images[other].dim != s.images[img].dim || s.images[other].depth != s.images[img].depth || s.images[other].arrayed != s.images[img].arrayed || s.images[other].ms != s.images[img].ms || s.images[other].sampled != s.images[img].sampled || s.images[other].format != s.images[img].format)
{
sampled_type_reusable = false;
break;
}
}
if (s.images[img].sampled == 2)
replacement_sampled = 0;
else if (sampled_type_reusable)
replacement_sampled = s.images[img].sampled_type_id;
else
replacement_sampled = nid++;
}
else
{
}
s.images[img].replacement_type = replacement;
s.images[img].replacement_sampled_type = replacement_sampled;
}
for (uint32_t img = 0; img < s.n_img; ++img)
{
if (!s.images[img].patchable)
continue;
uint32_t existing_pointer = 0;
if (s.images[img].replacement_sampled_type != 0 && s.images[img].replacement_sampled_type == s.images[img].sampled_type_id)
{
for (size_t scan = 5; scan < in_c;)
{
uint32_t sw = in[scan] >> 16;
uint32_t sop = in[scan] & 0xffffu;
if (!sw || scan + sw > in_c)
break;
if (sop == SpvOpTypePointer && sw >= 4 && in[scan + 2] == SpvStorageClassUniformConstant && in[scan + 3] == s.images[img].replacement_sampled_type)
{
existing_pointer = in[scan + 1];
break;
}
scan += sw;
}
}
s.images[img].replacement_pointer_type = existing_pointer ? existing_pointer : nid++;
}
uint32_t samp_nid = nid;
uint32_t qsize_nid = nid;
if (in_c > (UINT32_MAX-nid - n_patches * 10 - 32) / 8)
return false;
uint32_t new_bound = nid + n_patches * 10 + 32 + (uint32_t)in_c * 8;
SpvBuf ob;
if (!sb_init(&ob, in_c + 60 + (size_t)n_patches * 28))
return false;
uint32_t id_bound = new_bound;
bool *emitted_type = calloc(id_bound, sizeof(*emitted_type));
if (!emitted_type)
{
free(emitted_type);
sb_free(&ob);
return false;
}
bool mv_added = s.has_mv_cap;
bool ext_done = false;
uint32_t spv_version = in[1];
bool need_mv_ext =
!s.has_mv_cap &&
((spv_version >> 16) == 1) &&
(((spv_version >> 8) & 0xff) == 0);
bool types_done = false;
bool ep_done = false;
bool in_func = false;
sb_push_n(&ob, in, 5);
ob.w[3] = new_bound;
for (size_t i = 5; i < in_c; ) {
uint32_t op = in[i] & 0xffff; uint32_t wc = in[i] >> 16;
if (!wc || i + wc > in_c)
{
free(emitted_type);
sb_free(&ob);
return false;
}
if (!mv_added &&
op != SpvOpCapability)
{
uint32_t mv[] =
{
op_(SpvOpCapability, 2),
SpvCapabilityMultiView
};
sb_push_n(&ob, mv, 2);
mv_added = true;
}
if (!ext_done &&
need_mv_ext &&
op != SpvOpCapability)
{
uint32_t e[] =
{
op_(SpvOpExtension, 6),
0x5F565053, 
0x5F52484B, 
0x746C756D, 
0x65697669, 
0x00000077 
};
sb_push_n(&ob, e, 6);
ext_done = true;
}
if (emit_vi_decorate &&
op == SpvOpDecorate)
{
uint32_t d[] =
{
op_(SpvOpDecorate, 4),
new_vi_id,
SpvDecorationBuiltIn,
SpvBuiltInViewIndex
};
sb_push_n(&ob, d, 4);
uint32_t flat[] =
{
op_(SpvOpDecorate, 3),
new_vi_id,
SpvDecorationFlat
};
sb_push_n(&ob, flat, 3);
emit_vi_decorate = false;
}
if (op == SpvOpDecorate &&
wc >= 4)
{
sb_push_n(&ob, &in[i], wc);
i += wc;
continue;
}
if ((op == SpvOpTypeImage && wc >= 9) ||
(op == SpvOpTypePointer && wc >= 4))
{
bool definition_emitted = false;
for (size_t scan = 5; scan < ob.n;)
{
uint32_t sw = ob.w[scan] >> 16;
uint32_t sop = ob.w[scan] & 0xffffu;
if (!sw || scan + sw > ob.n)
break;
if (ob.w[scan + 1] == in[i + 1] &&
((op == SpvOpTypeImage && sop == SpvOpTypeImage && sw >= 9) ||
(op == SpvOpTypePointer && sop == SpvOpTypePointer && sw >= 4)))
{
definition_emitted = true;
break;
}
scan += sw;
}
if (definition_emitted)
{
i += wc;
continue;
}
}
if (op == SpvOpEntryPoint && !ep_done) {
ep_done = true;
if (new_vi_id != s.vi_var_id) {
sb_push(&ob, ((wc+1)<<16)|SpvOpEntryPoint);
sb_push_n(&ob, &in[i+1], wc-1);
sb_push(&ob, new_vi_id);
} else {
sb_push_n(&ob, &in[i], wc);
if (in[i + 1] < id_bound)
{
emitted_type[in[i + 1]] = true;
}
}
i += wc; continue;
}
if (op == SpvOpTypeFunction &&
wc >= 3)
{
uint32_t function_type_id = in[i + 1];
bool patched = false;
uint32_t w[64];
if (wc <= 64)
{
memcpy(w, &in[i], wc * sizeof(uint32_t));
for (uint32_t fn = 0; fn < s.n_function; ++fn)
{
if (s.functions[fn].type_id != function_type_id)
continue;
uint32_t first_param =
s.functions[fn].first_param;
for (uint32_t p = 0; p < s.n_param; ++p)
{
if (s.params[p].function_id !=
s.functions[fn].id)
continue;
if (p < first_param)
continue;
uint32_t function_param_index =
p - first_param;
uint32_t operand =
3 + function_param_index;
if (operand >= wc)
break;
uint32_t parameter_id =
s.params[p].id;
uint32_t replacement_pointer = 0;
for (uint32_t cidx = 0;
cidx < s.n_call;
++cidx)
{
const FsCallInfo *call =
&s.calls[cidx];
if (call->parameter_id != parameter_id)
continue;
for (uint32_t img = 0;
img < s.n_img;
++img)
{
const FsImageInfo *image =
&s.images[img];
if (image->owner_var !=
call->argument_var)
continue;
if (!image->stereo ||
!image->replacement_pointer_type)
continue;
replacement_pointer =
image->replacement_pointer_type;
break;
}
if (replacement_pointer)
break;
}
if (replacement_pointer &&
w[operand] != replacement_pointer)
{
w[operand] =
replacement_pointer;
patched = true;
}
}
}
if (patched)
{
sb_push_n(&ob, w, wc);
if (w[1] < id_bound)
emitted_type[w[1]] = true;
i += wc;
continue;
}
}
}
if (op == SpvOpTypeSampledImage &&
wc >= 3)
{
uint32_t sampled_id = in[i + 1];
uint32_t sampled_words[3] = {in[i], in[i + 1], in[i + 2]};
for (uint32_t img = 0; img < s.n_img; ++img)
{
FsImageInfo *image = &s.images[img];
if (!image->stereo ||
!image->replacement_type ||
image->sampled_type_id != sampled_id)
continue;
uint32_t replacement_image = image->replacement_type;
uint32_t existing_sampled_id = 0;
bool replacement_sampled_exists = false;
for (size_t scan = 5; scan < in_c;)
{
uint32_t sw = in[scan] >> 16;
uint32_t sop = in[scan] & 0xffffu;
if (!sw || scan + sw > in_c)
break;
if (scan != i && sop == SpvOpTypeSampledImage && sw >= 3 && in[scan + 2] == replacement_image)
{
existing_sampled_id = in[scan + 1];
replacement_sampled_exists = true;
break;
}
scan += sw;
}
if (!replacement_sampled_exists)
{
for (size_t scan = 5; scan < ob.n;)
{
uint32_t sw = ob.w[scan] >> 16;
uint32_t sop = ob.w[scan] & 0xffffu;
if (!sw || scan + sw > ob.n)
break;
if (sop == SpvOpTypeSampledImage && sw >= 3 && ob.w[scan + 2] == replacement_image)
{
existing_sampled_id = ob.w[scan + 1];
replacement_sampled_exists = true;
break;
}
scan += sw;
}
}
bool compatible_image_type = false;
for (size_t scan = 5; scan < in_c;)
{
uint32_t sw = in[scan] >> 16;
uint32_t sop = in[scan] & 0xffffu;
if (!sw || scan + sw > in_c)
break;
if (sop == SpvOpTypeImage && sw >= 9 && in[scan + 1] == in[i + 2])
{
uint32_t original_sampled = in[scan + 7];
for (size_t target = 5; target < in_c;)
{
uint32_t tw = in[target] >> 16;
uint32_t top = in[target] & 0xffffu;
if (!tw || target + tw > in_c)
break;
if (top == SpvOpTypeImage && tw >= 9 && in[target + 1] == replacement_image)
{
compatible_image_type =
original_sampled != 2 &&
in[target + 7] != 2 &&
in[scan + 2] == in[target + 2] &&
in[scan + 3] == in[target + 3] &&
in[scan + 5] == in[target + 5] &&
in[scan + 6] == in[target + 6] &&
in[scan + 7] == in[target + 7] &&
in[scan + 8] == in[target + 8];
break;
}
target += tw;
}
break;
}
scan += sw;
}
bool patch_original_sampled_type = image->replacement_sampled_type == sampled_id && image->sampled != 2;
if (!replacement_sampled_exists && (compatible_image_type || patch_original_sampled_type))
{
sampled_words[2] = replacement_image;
}
else
{
}
break;
}
sb_push_n(&ob, sampled_words, 3);
if (sampled_id < id_bound)
emitted_type[sampled_id] = true;
i += wc;
continue;
}
if (op == SpvOpTypePointer &&
wc >= 4)
{
uint32_t pointer_id = in[i + 1];
uint32_t storage = in[i + 2];
uint32_t target_type = in[i + 3];
bool target_defined = target_type < id_bound && emitted_type[target_type];
if (!target_defined)
{
for (size_t scan = 5; scan < ob.n;)
{
uint32_t sw = ob.w[scan] >> 16;
uint32_t sop = ob.w[scan] & 0xffffu;
if (!sw || scan + sw > ob.n)
break;
if (sop == SpvOpTypeImage && sw >= 9 && ob.w[scan + 1] == target_type)
{
target_defined = true;
break;
}
scan += sw;
}
}
if (!target_defined)
{
for (size_t scan = 5; scan < in_c;)
{
uint32_t sw = in[scan] >> 16;
uint32_t sop = in[scan] & 0xffffu;
if (!sw || scan + sw > in_c)
break;
if (sop == SpvOpTypeImage && sw >= 9 && in[scan + 1] == target_type)
{
bool target_is_stereo_replacement = false;
for (uint32_t img = 0; img < s.n_img; ++img)
{
if (s.images[img].stereo && s.images[img].replacement_type == target_type)
{
target_is_stereo_replacement = true;
break;
}
}
if (!target_is_stereo_replacement)
{
sb_push_n(&ob, &in[scan], sw);
if (target_type < id_bound)
emitted_type[target_type] = true;
target_defined = true;
}
break;
}
scan += sw;
}
}
if (!target_defined)
{
i += wc;
continue;
}
sb_push_n(&ob, &in[i], wc);
if (pointer_id < id_bound)
emitted_type[pointer_id] = true;
for (uint32_t img = 0; img < s.n_img; ++img)
{
FsImageInfo *image = &s.images[img];
if (!image->stereo ||
!image->replacement_pointer_type)
continue;
uint32_t replacement_target = 0;
if (target_type == image->id &&
image->replacement_type)
replacement_target = image->replacement_type;
else if (target_type == image->sampled_type_id &&
image->replacement_sampled_type)
replacement_target = image->replacement_sampled_type;
if (!replacement_target || image->replacement_pointer_type >= id_bound ||
replacement_target >= id_bound)
continue;
bool pointer_is_original = false;
for (size_t scan = 5; scan < in_c;)
{
uint32_t sw = in[scan] >> 16;
uint32_t sop = in[scan] & 0xffffu;
if (!sw || scan + sw > in_c)
break;
if (sop == SpvOpTypePointer && sw >= 4 && in[scan + 1] == image->replacement_pointer_type)
{
pointer_is_original = true;
break;
}
scan += sw;
}
if (pointer_is_original || emitted_type[image->replacement_pointer_type])
continue;
bool replacement_target_defined = replacement_target < id_bound && emitted_type[replacement_target];
for (size_t scan = 5; !replacement_target_defined && scan < ob.n;)
{
uint32_t sw = ob.w[scan] >> 16;
uint32_t sop = ob.w[scan] & 0xffffu;
if (!sw || scan + sw > ob.n)
break;
if (sop == SpvOpTypeImage && sw >= 9 && ob.w[scan + 1] == replacement_target)
replacement_target_defined = true;
scan += sw;
}
if (!replacement_target_defined)
continue;
uint32_t w[4] = {(4u << 16) | SpvOpTypePointer, image->replacement_pointer_type, storage, replacement_target};
sb_push_n(&ob, w, 4);
emitted_type[image->replacement_pointer_type] = true;
}
i += wc;
continue;
}
if (op == SpvOpTypeImage &&
wc >= 9 &&
in[i + 3] == SpvDim2D &&
in[i + 5] == 0)
{
int img_idx = -1;
for (uint32_t ii = 0; ii < s.n_img; ++ii)
{
if (s.images[ii].id == in[i + 1])
{
img_idx = (int)ii;
break;
}
}
if (img_idx >= 0)
{
FsImageInfo *img = &s.images[img_idx];
uint32_t existing =
fs_find_matching_image_type(
in,
in_c,
img->sampled_type,
img->dim,
img->depth,
1,
img->ms,
img->sampled,
img->format);
if (existing != 0 && existing < id_bound)
{
bool existing_defined = false;
for (size_t scan = 5; scan < in_c;)
{
uint32_t sw = in[scan] >> 16;
uint32_t sop = in[scan] & 0xffffu;
if (sw == 0 || scan + sw > in_c)
break;
if (sop == SpvOpTypeImage && sw >= 9 && in[scan + 1] == existing && in[scan + 5] == 1 && in[scan + 2] == img->sampled_type && in[scan + 3] == img->dim && in[scan + 4] == img->depth && in[scan + 6] == img->ms && in[scan + 7] == img->sampled && in[scan + 8] == img->format)
existing_defined = true;
scan += sw;
}
if (existing == img->id)
existing_defined = false;
uint32_t existing_sampled = existing_defined ? fs_find_matching_sampled_image(in, in_c, existing) : 0;
bool sampled_defined = false;
for (size_t scan = 5; scan < in_c;)
{
uint32_t sw = in[scan] >> 16;
uint32_t sop = in[scan] & 0xffffu;
if (sw == 0 || scan + sw > in_c)
break;
if (sop == SpvOpTypeSampledImage && sw >= 3 && in[scan + 1] == existing_sampled && in[scan + 2] == existing)
sampled_defined = true;
scan += sw;
}
if (existing_defined && existing_sampled != 0 && sampled_defined)
{
uint32_t original_pointer_target = 0;
for (size_t scan = 5; scan < in_c;)
{
uint32_t sw = in[scan] >> 16;
uint32_t sop = in[scan] & 0xffffu;
if (sw == 0 || scan + sw > in_c)
break;
if (sop == SpvOpTypePointer && sw >= 4 && in[scan + 1] == img->pointer_type)
{
original_pointer_target = in[scan + 3];
break;
}
scan += sw;
}
uint32_t wanted_pointer_target = original_pointer_target == img->sampled_type_id ? existing_sampled : existing;
uint32_t existing_pointer = 0;
for (size_t scan = 5; scan < in_c;)
{
uint32_t sw = in[scan] >> 16;
uint32_t sop = in[scan] & 0xffffu;
if (sw == 0 || scan + sw > in_c)
break;
if (sop == SpvOpTypePointer && sw >= 4 && in[scan + 2] == SpvStorageClassUniformConstant && in[scan + 3] == wanted_pointer_target)
{
existing_pointer = in[scan + 1];
break;
}
scan += sw;
}
if (existing_pointer != 0 && existing_pointer < id_bound && wanted_pointer_target != 0)
{
img->replacement_type = existing;
img->replacement_sampled_type = existing_sampled;
img->replacement_pointer_type = existing_pointer;
for (uint32_t copy = 0; copy < s.n_img; ++copy)
{
if (s.images[copy].sampled_type_id != s.images[img_idx].sampled_type_id ||
s.images[copy].sampled_type != s.images[img_idx].sampled_type ||
s.images[copy].dim != s.images[img_idx].dim ||
s.images[copy].depth != s.images[img_idx].depth ||
s.images[copy].arrayed != s.images[img_idx].arrayed ||
s.images[copy].ms != s.images[img_idx].ms ||
s.images[copy].sampled != s.images[img_idx].sampled ||
s.images[copy].format != s.images[img_idx].format)
continue;
s.images[copy].replacement_type = existing;
s.images[copy].replacement_sampled_type = existing_sampled;
s.images[copy].replacement_pointer_type = existing_pointer;
}
sb_push_n(&ob, &in[i], wc);
if (in[i + 1] < id_bound)
emitted_type[in[i + 1]] = true;
i += wc;
continue;
}
}
}
}
bool patch_this_type = false;
int patch_img_idx = -1;
for (uint32_t img = 0; img < s.n_img; ++img)
{
if (s.images[img].id != in[i + 1])
continue;
if (s.images[img].stereo)
{
patch_this_type = true;
patch_img_idx = (int)img;
}
}
for (uint32_t img = 0; img < s.n_img; ++img)
{
if (s.images[img].stereo)
{
}
}
if (!patch_this_type)
{
sb_push_n(&ob, &in[i], wc);
if (in[i + 1] < id_bound)
{
emitted_type[in[i + 1]] = true;
}
i += wc;
continue;
}
sb_push_n(&ob, &in[i], wc);
if (in[i + 1] < id_bound)
{
emitted_type[in[i + 1]] = true;
}
if (patch_img_idx < 0)
{
i += wc;
continue;
}
uint32_t new_array_type = s.images[patch_img_idx].replacement_type;
uint32_t duplicate_array_type = fs_find_matching_image_type(in, in_c, s.images[patch_img_idx].sampled_type, s.images[patch_img_idx].dim, s.images[patch_img_idx].depth, 1, s.images[patch_img_idx].ms, s.images[patch_img_idx].sampled, s.images[patch_img_idx].format);
if (duplicate_array_type && duplicate_array_type != s.images[patch_img_idx].id)
{
if (new_array_type != duplicate_array_type)
{
s.images[patch_img_idx].replacement_type = duplicate_array_type;
for (uint32_t copy = 0; copy < s.n_img; ++copy)
{
if (s.images[copy].sampled_type_id != s.images[patch_img_idx].sampled_type_id ||
s.images[copy].sampled_type != s.images[patch_img_idx].sampled_type ||
s.images[copy].dim != s.images[patch_img_idx].dim ||
s.images[copy].depth != s.images[patch_img_idx].depth ||
s.images[copy].arrayed != s.images[patch_img_idx].arrayed ||
s.images[copy].ms != s.images[patch_img_idx].ms ||
s.images[copy].sampled != s.images[patch_img_idx].sampled ||
s.images[copy].format != s.images[patch_img_idx].format)
continue;
s.images[copy].replacement_type = duplicate_array_type;
}
new_array_type = duplicate_array_type;
}
i += wc;
continue;
}
if (new_array_type == 0 || (new_array_type < id_bound && emitted_type[new_array_type]))
{
i += wc;
continue;
}
uint32_t new_sampled_type =
s.images[patch_img_idx].replacement_sampled_type;
uint32_t w[9];
if (wc != 9)
{
sb_push_n(&ob, &in[i], wc);
if (in[i + 1] < id_bound)
emitted_type[in[i + 1]] = true;
i += wc;
continue;
}
memcpy(w, &in[i], sizeof(w));
w[1] = new_array_type;
w[5] = 1;
sb_push_n(&ob, w, wc);
if (w[1] < id_bound)
{
emitted_type[w[1]] = true;
}
if (new_sampled_type != 0 &&
s.images[patch_img_idx].sampled != 2 &&
new_sampled_type < id_bound &&
!emitted_type[new_sampled_type])
{
uint32_t existing_sampled =
fs_find_matching_sampled_image(
in,
in_c,
new_array_type);
bool sampled_is_original = false;
for (size_t j = 5; j < in_c;)
{
uint32_t wcj = in[j] >> 16;
uint32_t opj = in[j] & 0xffff;
if (!wcj || j + wcj > in_c)
break;
if (opj == SpvOpTypeSampledImage &&
wcj >= 3 &&
in[j + 1] == new_sampled_type)
{
sampled_is_original = true;
break;
}
j += wcj;
}
if (sampled_is_original)
{
}
else if (existing_sampled == new_sampled_type)
{
}
else
{
uint32_t sampled[] =
{
(3u << 16) | SpvOpTypeSampledImage,
new_sampled_type,
new_array_type
};
sb_push_n(&ob, sampled, 3);
emitted_type[new_sampled_type] = true;
}
}
i += wc;
continue;
}
if (op == SpvOpFunctionParameter &&
wc >= 3)
{
uint32_t parameter_type = in[i + 1];
uint32_t parameter_id = in[i + 2];
uint32_t replacement_pointer = 0;
uint32_t argument_var = 0;
for (uint32_t p = 0; p < s.n_param; ++p)
{
if (s.params[p].id != parameter_id)
continue;
for (uint32_t cidx = 0; cidx < s.n_call; ++cidx)
{
const FsCallInfo *call = &s.calls[cidx];
if (call->parameter_id != parameter_id)
continue;
argument_var = call->argument_var;
for (uint32_t img = 0; img < s.n_img; ++img)
{
const FsImageInfo *image = &s.images[img];
if (image->owner_var != argument_var)
continue;
if (!image->stereo ||
!image->replacement_pointer_type ||
!image->replacement_sampled_type)
continue;
replacement_pointer =
image->replacement_pointer_type;
break;
}
if (replacement_pointer)
break;
}
if (replacement_pointer)
break;
}
if (replacement_pointer)
{
uint32_t w[3];
memcpy(w, &in[i], sizeof(w));
w[1] = replacement_pointer;
sb_push_n(&ob, w, wc);
i += wc;
continue;
}
sb_push_n(&ob, &in[i], wc);
i += wc;
continue;
}
if (op == SpvOpVariable &&
wc >= 4)
{
bool patched = false;
uint32_t variable_id = in[i + 2];
uint32_t variable_pointer_type = in[i + 1];
uint32_t variable_target_type = 0;
if (in[i + 3] == SpvStorageClassUniformConstant)
{
for (size_t scan = 5; scan < in_c;)
{
uint32_t sw = in[scan] >> 16;
uint32_t sop = in[scan] & 0xffffu;
if (sw == 0 || scan + sw > in_c)
break;
if (sop == SpvOpTypePointer &&
sw >= 4 &&
in[scan + 1] == variable_pointer_type)
{
variable_target_type = in[scan + 3];
break;
}
scan += sw;
}
uint32_t call_pointer_type = 0;
uint32_t call_parameter_id = 0;
uint32_t call_function_id = 0;
uint32_t call_parameter_index = 0;
for (uint32_t p = 0; p < s.n_param; ++p)
{
for (uint32_t cidx = 0; cidx < s.n_call; ++cidx)
{
const FsCallInfo *call = &s.calls[cidx];
if (call->argument_var != variable_id ||
call->parameter_id != s.params[p].id)
continue;
call_parameter_id = s.params[p].id;
call_function_id = s.params[p].function_id;
call_parameter_index = s.params[p].index;
break;
}
if (call_parameter_id)
break;
}
if (call_parameter_id)
{
uint32_t function_type_id = 0;
for (size_t scan = 5; scan < in_c;)
{
uint32_t sw = in[scan] >> 16;
uint32_t sop = in[scan] & 0xffffu;
if (sw == 0 || scan + sw > in_c)
break;
if (sop == SpvOpFunction &&
sw >= 5 &&
in[scan + 2] == call_function_id)
{
function_type_id = in[scan + 4];
break;
}
scan += sw;
}
if (function_type_id)
{
size_t scan = 5;
while (scan < ob.n)
{
uint32_t sw = ob.w[scan] >> 16;
uint32_t sop = ob.w[scan] & 0xffffu;
if (sw == 0 || scan + sw > ob.n)
break;
if (sop == SpvOpTypeFunction &&
sw >= 3 &&
ob.w[scan + 1] == function_type_id &&
call_parameter_index + 3 < sw)
{
call_pointer_type = ob.w[scan + 3 + call_parameter_index];
break;
}
scan += sw;
}
}
}
for (uint32_t img = 0; img < s.n_img; ++img)
{
FsImageInfo *image = &s.images[img];
if (!image->stereo ||
!image->replacement_pointer_type)
continue;
bool owner_match = (image->owner_var == variable_id);
bool target_is_image = (variable_target_type == image->id);
bool target_is_sampled = (variable_target_type == image->sampled_type_id &&
image->replacement_sampled_type);
if (owner_match)
{
if (variable_target_type != 0 &&
!target_is_image &&
!target_is_sampled)
continue;
}
else
{
if (variable_target_type == 0 ||
(!target_is_image && !target_is_sampled))
continue;
}
uint32_t replacement_target = target_is_sampled ?
image->replacement_sampled_type :
image->replacement_type;
if (!replacement_target)
continue;
if (!call_pointer_type &&
target_is_sampled &&
replacement_target == variable_target_type)
{
continue;
}
if (call_pointer_type)
{
uint32_t rewritten_parameter_type = 0;
for (size_t scan = 5; scan < ob.n;)
{
uint32_t sw = ob.w[scan] >> 16;
uint32_t sop = ob.w[scan] & 0xffffu;
if (sw == 0 || scan + sw > ob.n)
break;
if (sop == SpvOpTypeFunction &&
sw >= 3 &&
ob.w[scan + 2] == call_pointer_type)
{
rewritten_parameter_type = ob.w[scan + 3];
break;
}
scan += sw;
}
if (rewritten_parameter_type == 0)
rewritten_parameter_type = call_pointer_type;
if (rewritten_parameter_type >= id_bound)
continue;
uint32_t w[4];
memcpy(w, &in[i], sizeof(w));
w[1] = rewritten_parameter_type;
sb_push_n(&ob, w, wc);
patched = true;
break;
}
if (image->replacement_pointer_type >= id_bound ||
replacement_target >= id_bound)
{
continue;
}
if (!emitted_type[replacement_target])
{
for (size_t scan = 5; scan < in_c;)
{
uint32_t sw = in[scan] >> 16;
uint32_t sop = in[scan] & 0xffffu;
if (!sw || scan + sw > in_c)
break;
if (sop == SpvOpTypeImage &&
sw >= 9 &&
in[scan + 1] == replacement_target)
{
sb_push_n(&ob, &in[scan], sw);
emitted_type[replacement_target] = true;
break;
}
scan += sw;
}
}
if (!emitted_type[image->replacement_pointer_type])
{
for (size_t scan = 5; scan < in_c;)
{
uint32_t sw = in[scan] >> 16;
uint32_t sop = in[scan] & 0xffffu;
if (!sw || scan + sw > in_c)
break;
if (sop == SpvOpTypePointer &&
sw >= 4 &&
in[scan + 1] == image->replacement_pointer_type &&
in[scan + 2] == SpvStorageClassUniformConstant &&
in[scan + 3] == replacement_target)
{
sb_push_n(&ob, &in[scan], sw);
emitted_type[image->replacement_pointer_type] = true;
break;
}
scan += sw;
}
}
if (!emitted_type[image->replacement_pointer_type])
continue;
uint32_t w[4];
memcpy(w, &in[i], sizeof(w));
w[1] = image->replacement_pointer_type;
sb_push_n(&ob, w, wc);
patched = true;
break;
}
}
if (!patched)
{
sb_push_n(&ob, &in[i], wc);
}
i += wc;
continue;
}
if (op == SpvOpFunction && !types_done) {
types_done = true;
in_func = true;
if (!s.int_id) {
uint32_t w[]={(4u<<16)|SpvOpTypeInt, new_int_id, 32, 1};
sb_push_n(&ob,w,4); }
if (!s.v3float_id) {
uint32_t w[]={(4u<<16)|SpvOpTypeVector, new_v3f_id, s.float_id, 3};
sb_push_n(&ob,w,4); }
if (!s.v3int_id)
{
uint32_t w[]={(4u<<16)|SpvOpTypeVector, new_v3i_id, new_int_id, 3};
sb_push_n(&ob,w,4);
}
bool v3u_type_defined = false;
if (s.uint_id && new_v3u_id)
{
for (size_t scan = 5; scan < in_c;)
{
uint32_t sw = in[scan] >> 16;
uint32_t sop = in[scan] & 0xffffu;
if (!sw || scan + sw > in_c)
break;
if (sop == SpvOpTypeVector && sw >= 4 && in[scan + 1] == new_v3u_id && in[scan + 2] == s.uint_id && in[scan + 3] == 3)
{
v3u_type_defined = true;
break;
}
scan += sw;
}
if (!v3u_type_defined)
{
uint32_t w[]={(4u<<16)|SpvOpTypeVector,new_v3u_id,s.uint_id,3};
sb_push_n(&ob,w,4);
}
}
if (!s.ptr_int_in_id)
{
uint32_t w[]={(4u<<16)|32,new_pin_id,1,new_int_id};
sb_push_n(&ob,w,4);
}
if (emit_vi_variable) {
uint32_t w[]={(4u<<16)|SpvOpVariable, new_pin_id, new_vi_id, SpvStorageClassInput};
sb_push_n(&ob,w,4); }
sb_push_n(&ob, &in[i], wc);
if (in[i + 1] < id_bound)
{
emitted_type[in[i + 1]] = true;
}
i += wc; continue;
}
if (op == SpvOpTypeArray &&
wc >= 4)
{
int img = -1;
for (uint32_t j = 0; j < s.n_img; ++j)
{
if (!s.images[j].stereo ||
!s.images[j].replacement_type ||
s.images[j].id != in[i + 2])
continue;
img = (int)j;
break;
}
if (img >= 0)
{
uint32_t w[4];
memcpy(w, &in[i], sizeof(w));
w[2] = s.images[img].replacement_type;
sb_push_n(&ob, w, wc);
if (w[1] < id_bound)
emitted_type[w[1]] = true;
i += wc;
continue;
}
}
if (op == SpvOpVariable &&
wc >= 4 &&
in[i + 3] == SpvStorageClassUniformConstant)
{
uint32_t pointer_target = 0;
for (size_t scan = 5; scan < in_c;)
{
uint32_t sw = in[scan] >> 16;
uint32_t sop = in[scan] & 0xffffu;
if (!sw || scan + sw > in_c)
break;
if (sop == SpvOpTypePointer &&
sw >= 4 &&
in[scan + 1] == in[i + 1])
{
pointer_target = in[scan + 3];
break;
}
scan += sw;
}
bool variable_rewritten = false;
for (uint32_t j = 0; j < s.n_img; ++j)
{
FsImageInfo *image = &s.images[j];
if (!image->stereo ||
image->owner_var != in[i + 2] ||
!image->replacement_pointer_type ||
!image->replacement_type ||
pointer_target != image->id)
continue;
uint32_t *w = malloc(wc * sizeof(uint32_t));
if (!w)
break;
memcpy(w, &in[i], wc * sizeof(uint32_t));
w[1] = image->replacement_pointer_type;
sb_push_n(&ob, w, wc);
free(w);
variable_rewritten = true;
break;
}
if (variable_rewritten)
{
i += wc;
continue;
}
}
if (op == SpvOpFunction) in_func = true;
if (in_func &&
(op == SpvOpAccessChain ||
op == SpvOpInBoundsAccessChain ||
op == SpvOpPtrAccessChain ||
op == SpvOpInBoundsPtrAccessChain ||
op == SpvOpCopyObject) &&
wc >= 4)
{
uint32_t pointer_target = 0;
for (size_t scan = 5; scan < in_c;)
{
uint32_t sw = in[scan] >> 16;
uint32_t sop = in[scan] & 0xffffu;
if (!sw || scan + sw > in_c)
break;
if (sop == SpvOpTypePointer &&
sw >= 4 &&
in[scan + 1] == in[i + 1])
{
pointer_target = in[scan + 3];
break;
}
scan += sw;
}
uint32_t replacement_pointer = 0;
for (uint32_t j = 0; j < s.n_img; ++j)
{
FsImageInfo *image = &s.images[j];
if (!image->stereo ||
!image->replacement_type ||
!image->replacement_pointer_type ||
image->id != pointer_target)
continue;
replacement_pointer = image->replacement_pointer_type;
break;
}
if (replacement_pointer && in[i + 1] != replacement_pointer)
{
uint32_t replacement_target = 0;
for (size_t scan = 5; scan < in_c;)
{
uint32_t sw = in[scan] >> 16;
uint32_t sop = in[scan] & 0xffffu;
if (!sw || scan + sw > in_c)
break;
if (sop == SpvOpTypePointer && sw >= 4 && in[scan + 1] == replacement_pointer)
{
replacement_target = in[scan + 3];
break;
}
scan += sw;
}
if (replacement_target != 0)
{
uint32_t *w = malloc(wc * sizeof(uint32_t));
if (!w)
{
sb_push_n(&ob, &in[i], wc);
i += wc;
continue;
}
memcpy(w, &in[i], wc * sizeof(uint32_t));
w[1] = replacement_pointer;
sb_push_n(&ob, w, wc);
free(w);
i += wc;
continue;
}
}
}
if (in_func &&
op == SpvOpLoad &&
wc >= 4)
{
uint32_t *w = malloc(wc * sizeof(uint32_t));
if (!w)
{
sb_push_n(&ob, &in[i], wc);
if (in[i + 1] < id_bound)
emitted_type[in[i + 1]] = true;
i += wc;
continue;
}
memcpy(w, &in[i], wc * sizeof(uint32_t));
uint32_t pointer_id = in[i + 3];
uint32_t pointer_type = 0;
uint32_t pointer_target = 0;
for (size_t scan = 5; scan < ob.n;)
{
uint32_t sw = ob.w[scan] >> 16;
uint32_t sop = ob.w[scan] & 0xffffu;
if (!sw || scan + sw > ob.n)
break;
if ((sop == SpvOpVariable || sop == SpvOpFunctionParameter) && sw >= 3 && ob.w[scan + 2] == pointer_id)
{
pointer_type = ob.w[scan + 1];
break;
}
if ((sop == SpvOpAccessChain ||
sop == SpvOpInBoundsAccessChain ||
sop == SpvOpPtrAccessChain ||
sop == SpvOpInBoundsPtrAccessChain) &&
sw >= 4 &&
ob.w[scan + 2] == pointer_id)
{
pointer_type = ob.w[scan + 1];
break;
}
if (sop == SpvOpCopyObject && sw >= 4 && ob.w[scan + 2] == pointer_id)
{
pointer_type = ob.w[scan + 1];
break;
}
scan += sw;
}
if (!pointer_type)
{
for (size_t scan = 5; scan < in_c;)
{
uint32_t sw = in[scan] >> 16;
uint32_t sop = in[scan] & 0xffffu;
if (!sw || scan + sw > in_c)
break;
if ((sop == SpvOpVariable ||
sop == SpvOpFunctionParameter ||
sop == SpvOpAccessChain ||
sop == SpvOpInBoundsAccessChain ||
sop == SpvOpPtrAccessChain ||
sop == SpvOpInBoundsPtrAccessChain ||
sop == SpvOpCopyObject) &&
sw >= (sop == SpvOpFunctionParameter ? 3u : 4u) &&
in[scan + 2] == pointer_id)
{
pointer_type = in[scan + 1];
break;
}
scan += sw;
}
}
if (pointer_type)
{
for (size_t scan = 5; scan < ob.n;)
{
uint32_t sw = ob.w[scan] >> 16;
uint32_t sop = ob.w[scan] & 0xffffu;
if (!sw || scan + sw > ob.n)
break;
if (sop == SpvOpTypePointer && sw >= 4 && ob.w[scan + 1] == pointer_type)
{
pointer_target = ob.w[scan + 3];
break;
}
scan += sw;
}
if (!pointer_target)
{
for (size_t scan = 5; scan < in_c;)
{
uint32_t sw = in[scan] >> 16;
uint32_t sop = in[scan] & 0xffffu;
if (!sw || scan + sw > in_c)
break;
if (sop == SpvOpTypePointer && sw >= 4 && in[scan + 1] == pointer_type)
{
pointer_target = in[scan + 3];
break;
}
scan += sw;
}
}
}
for (uint32_t j = 0; j < s.n_img; ++j)
{
FsImageInfo *image = &s.images[j];
if (!image->stereo ||
image->owner_var != pointer_id ||
!image->replacement_pointer_type)
continue;
if (pointer_target == image->sampled_type_id &&
image->replacement_sampled_type)
{
pointer_type = image->replacement_pointer_type;
pointer_target = image->replacement_sampled_type;
break;
}
if (pointer_target == image->id &&
image->replacement_type)
{
pointer_type = image->replacement_pointer_type;
pointer_target = image->replacement_type;
break;
}
}
bool load_rewritten = false;
if (pointer_type && pointer_target)
{
w[1] = pointer_target;
load_rewritten = true;
}
if (!load_rewritten &&
pointer_target != 0)
{
int img = -1;
uint32_t replacement_load_type = 0;
for (uint32_t j = 0; j < s.n_img; ++j)
{
FsImageInfo *image = &s.images[j];
if (!image->stereo)
continue;
if (image->replacement_type &&
image->replacement_pointer_type &&
(image->replacement_type == pointer_target ||
image->id == pointer_target))
{
img = (int)j;
replacement_load_type = image->replacement_type;
break;
}
if (image->replacement_sampled_type &&
image->replacement_pointer_type &&
(image->replacement_sampled_type == pointer_target ||
image->sampled_type_id == pointer_target))
{
img = (int)j;
replacement_load_type = image->replacement_sampled_type;
break;
}
}
if (img >= 0 &&
replacement_load_type != 0 &&
w[1] != replacement_load_type)
{
FsImageInfo *image = &s.images[img];
w[1] = replacement_load_type;
load_rewritten = true;
}
}
if (pointer_type &&
pointer_target &&
w[1] != pointer_target)
{
bool pointer_decl_matches = false;
for (size_t scan = 5; scan < ob.n;)
{
uint32_t sw = ob.w[scan] >> 16;
uint32_t sop = ob.w[scan] & 0xffffu;
if (!sw || scan + sw > ob.n)
break;
if (sop == SpvOpTypePointer && sw >= 4 && ob.w[scan + 1] == pointer_type && ob.w[scan + 3] == pointer_target)
{
pointer_decl_matches = true;
break;
}
scan += sw;
}
if (pointer_decl_matches)
{
w[1] = pointer_target;
load_rewritten = true;
}
}
sb_push_n(&ob, w, wc);
free(w);
i += wc;
continue;
}
if (in_func &&
op == SpvOpImageTexelPointer &&
wc >= 6)
{
uint32_t image_id = in[i + 3];
uint32_t coord_id = in[i + 4];
int img_idx = -1;
for (uint32_t j = 0; j < s.n_img; ++j)
{
FsImageInfo *image = &s.images[j];
if (!image->stereo ||
!image->replacement_type ||
(image->owner_var != UINT32_MAX && image->owner_var != image_id && image->id != image_id))
continue;
img_idx = (int)j;
break;
}
if (img_idx >= 0 && new_vi_id && new_v3u_id && new_v3i_id)
{
uint32_t coord_type = 0;
for (size_t scan = 5; scan < in_c;)
{
uint32_t sw = in[scan] >> 16;
uint32_t sop = in[scan] & 0xffffu;
if (!sw || scan + sw > in_c)
{
break;
}
if (sw >= 3 &&
in[scan + 2] == coord_id)
{
if (sop == SpvOpCompositeConstruct ||
sop == SpvOpLoad ||
sop == SpvOpCopyObject ||
sop == SpvOpVectorShuffle ||
sop == SpvOpPhi ||
sop == SpvOpSelect ||
sop == SpvOpConstantComposite ||
sop == SpvOpSpecConstantComposite)
{
coord_type = in[scan + 1];
break;
}
}
scan += sw;
}
uint32_t coord_component_type = 0;
uint32_t coord_components = 0;
for (size_t scan = 5; scan < in_c;)
{
uint32_t sw = in[scan] >> 16;
uint32_t sop = in[scan] & 0xffffu;
if (!sw || scan + sw > in_c)
break;
if (sop == SpvOpTypeVector &&
sw >= 4 &&
in[scan + 1] == coord_type)
{
coord_component_type = in[scan + 2];
coord_components = in[scan + 3];
break;
}
scan += sw;
}
bool coord_uint = coord_component_type != 0 &&
coord_component_type == s.uint_id;
bool coord_int = coord_component_type != 0 &&
(coord_component_type == new_int_id ||
coord_component_type == s.int_id);
uint32_t coord_v3_type = coord_uint ? new_v3u_id :
coord_int ? new_v3i_id : 0;
if (coord_components == 2 && coord_v3_type)
{
uint32_t view_id = samp_nid++;
uint32_t x_id = samp_nid++;
uint32_t y_id = samp_nid++;
uint32_t layer_id = view_id;
uint32_t new_coord_id = samp_nid++;
uint32_t load_view[] = {(4u << 16) | SpvOpLoad, new_int_id, view_id, new_vi_id};
sb_push_n(&ob, load_view, 4);
if (coord_uint)
{
layer_id = samp_nid++;
uint32_t convert_layer[] = {(4u << 16) | SpvOpBitcast, coord_component_type, layer_id, view_id};
sb_push_n(&ob, convert_layer, 4);
}
else if (coord_int)
{
layer_id = view_id;
}
uint32_t extract_x[] = {(5u << 16) | SpvOpCompositeExtract, coord_component_type, x_id, coord_id, 0};
uint32_t extract_y[] = {(5u << 16) | SpvOpCompositeExtract, coord_component_type, y_id, coord_id, 1};
sb_push_n(&ob, extract_x, 5);
sb_push_n(&ob, extract_y, 5);
uint32_t construct_coord[] = {(6u << 16) | SpvOpCompositeConstruct, coord_v3_type, new_coord_id, x_id, y_id, layer_id};
sb_push_n(&ob, construct_coord, 6);
uint32_t w[6];
memcpy(w, &in[i], sizeof(w));
w[4] = new_coord_id;
sb_push_n(&ob, w, wc);
i += wc;
continue;
}
}
}
if (op >= SpvOpImageSampleImplicitLod &&
op <= SpvOpImageSampleProjDrefExplicitLod &&
wc >= 5)
{
}
if (in_func && op == SpvOpStore && wc >= 3)
{
uint32_t target = in[i+1];
int vi = fs_var_index(&s, target);
if (vi >= 0)
{
}
else
{
}
}
if (in_func && wc >= 5 &&
(op == SpvOpImageSampleImplicitLod ||
op == SpvOpImageSampleExplicitLod ||
op == SpvOpImageSampleDrefImplicitLod ||
op == SpvOpImageSampleDrefExplicitLod ||
op == SpvOpImageGather ||
op == SpvOpImageDrefGather))
{
uint32_t coord_id = in[i+4];
int coord_type = -1;
for (uint32_t v = 0; v < s.n_var; ++v)
{
if (s.vars[v].id == coord_id)
{
coord_type = s.vars[v].type;
break;
}
}
uint32_t descriptor_var = 0;
int load =
fs_find_load(
&s,
in[i+3]);
if (load < 0)
{
for (size_t scan = 5; scan < in_c;)
{
uint32_t sw = in[scan] >> 16;
uint32_t sop = in[scan] & 0xffffu;
if (sw == 0)
break;
if (sop == SpvOpLoad && sw >= 4 && in[scan + 2] == in[i + 3])
{
uint32_t ptr = in[scan + 3];
for (uint32_t vv = 0; vv < s.n_var; ++vv)
{
if (s.vars[vv].id != ptr)
continue;
if (s.vars[vv].storage != SpvStorageClassUniformConstant)
continue;
descriptor_var = s.vars[vv].id;
break;
}
break;
}
scan += sw;
}
}
if (load >= 0)
{
descriptor_var =
s.loads[load].owner_var;
int vi =
fs_var_index(
&s,
descriptor_var);
if (vi >= 0)
{
}
}
if (load < 0 && descriptor_var)
{
int vi = fs_var_index(&s, descriptor_var);
}
if (!descriptor_var)
{
uint32_t sampled_image_id = in[i+3];
uint32_t image_id = 0;
for (size_t scan = 5; scan < in_c;)
{
uint32_t sw = in[scan] >> 16;
uint32_t sop = in[scan] & 0xffffu;
if (sw == 0 || scan + sw > in_c)
break;
if (sop == SpvOpSampledImage &&
sw >= 5 &&
in[scan + 2] == sampled_image_id)
{
image_id = in[scan + 3];
break;
}
scan += sw;
}
if (image_id)
{
for (size_t scan = 5; scan < in_c;)
{
uint32_t sw = in[scan] >> 16;
uint32_t sop = in[scan] & 0xffffu;
if (sw == 0 || scan + sw > in_c)
break;
if (sop == SpvOpLoad &&
sw >= 4 &&
in[scan + 2] == image_id)
{
uint32_t ptr = in[scan + 3];
for (uint32_t vv = 0; vv < s.n_var; ++vv)
{
if (s.vars[vv].id != ptr)
continue;
if (s.vars[vv].storage != SpvStorageClassUniformConstant)
continue;
descriptor_var = ptr;
break;
}
if (descriptor_var)
break;
}
scan += sw;
}
}
}
if (!descriptor_var)
{
sb_push_n(&ob, &in[i], wc);
if (in[i + 1] < id_bound)
{
emitted_type[in[i + 1]] = true;
}
i += wc;
continue;
}
int image_dim = -1;
int image_idx = -1;
uint32_t loaded_image_type = 0;
for (size_t scan = 5; scan < in_c;)
{
uint32_t sw = in[scan] >> 16;
uint32_t sop = in[scan] & 0xffffu;
if (sw == 0 || scan + sw > in_c)
break;
if (((sop == SpvOpLoad && sw >= 4) ||
(sop == SpvOpSampledImage && sw >= 5)) &&
in[scan + 2] == in[i + 3])
{
loaded_image_type = in[scan + 1];
break;
}
scan += sw;
}
uint32_t image_type_id = 0;
for (size_t scan = 5; scan < in_c;)
{
uint32_t sw = in[scan] >> 16;
uint32_t sop = in[scan] & 0xffffu;
if (sw == 0 || scan + sw > in_c)
break;
if (sop == SpvOpTypeSampledImage && sw >= 3 && in[scan + 1] == loaded_image_type)
{
image_type_id = in[scan + 2];
break;
}
if (sop == SpvOpTypeImage && sw >= 9 && in[scan + 1] == loaded_image_type)
{
image_type_id = loaded_image_type;
break;
}
scan += sw;
}
for (size_t scan = 5; scan < in_c;)
{
uint32_t sw = in[scan] >> 16;
uint32_t sop = in[scan] & 0xffffu;
if (sw == 0 || scan + sw > in_c)
break;
if (sop == SpvOpTypeImage && sw >= 9 && in[scan + 1] == image_type_id)
{
image_dim = (int)in[scan + 3];
break;
}
scan += sw;
}
for (uint32_t img = 0; img < s.n_img; ++img)
{
if (s.images[img].owner_var == descriptor_var ||
s.images[img].id == image_type_id ||
s.images[img].replacement_type == image_type_id)
{
image_idx = (int)img;
break;
}
}
int replacement_2d = 0;
if (image_idx >= 0 &&
s.images[image_idx].dim == SpvDim2D &&
s.images[image_idx].replacement_type)
replacement_2d = 1;
if (!fs_should_patch_sample(&s, h, descriptor_var) && !replacement_2d)
{
sb_push_n(&ob, &in[i], wc);
if (in[i + 1] < id_bound)
{
emitted_type[in[i + 1]] = true;
}
i += wc;
continue;
}
if (image_dim != SpvDim2D)
{
sb_push_n(&ob, &in[i], wc);
if (in[i + 1] < id_bound)
{
emitted_type[in[i + 1]] = true;
}
i += wc;
continue;
}
int vi = fs_var_index(&s, descriptor_var);
int sampled_type = -1;
for (uint32_t v = 0; v < s.n_var; ++v)
{
if (s.vars[v].id == descriptor_var)
{
sampled_type = s.vars[v].type;
break;
}
}
fs_dump_descriptor_chain(
&s,
in,
in_c,
descriptor_var);
uint32_t id_lv = samp_nid++;
uint32_t id_cvt = samp_nid++;
uint32_t id_u = samp_nid++;
uint32_t id_v = samp_nid++;
uint32_t id_c3 = samp_nid++;
{ uint32_t w[]={(4u<<16)|SpvOpLoad, new_int_id, id_lv, new_vi_id};
sb_push_n(&ob,w,4); }
{ uint32_t w[]={(4u<<16)|SpvOpConvertSToF, s.float_id, id_cvt, id_lv};
sb_push_n(&ob,w,4); }
{ uint32_t w[]={(5u<<16)|SpvOpCompositeExtract, s.float_id, id_u, coord_id, 0};
sb_push_n(&ob,w,5); }
{ uint32_t w[]={(5u<<16)|SpvOpCompositeExtract, s.float_id, id_v, coord_id, 1};
sb_push_n(&ob,w,5); }
{ uint32_t w[]={(6u<<16)|SpvOpCompositeConstruct, new_v3f_id, id_c3, id_u, id_v, id_cvt};
sb_push_n(&ob,w,6); }
sb_push(&ob, in[i]); 
sb_push(&ob, in[i+1]); 
sb_push(&ob, in[i+2]); 
sb_push(&ob, in[i+3]); 
sb_push(&ob, id_c3); 
if (wc > 5) sb_push_n(&ob, &in[i+5], wc-5); 
size_t out = ob.n - wc;
i += wc; continue;
}
if (in_func &&
op == SpvOpImage &&
wc >= 4)
{
if ((in[i + 2] == 170 && in[i + 3] == 169) ||
(in[i + 2] == 38 && in[i + 3] == 37))
{
}
if (wc != 4)
{
sb_push_n(&ob, &in[i], wc);
if (in[i + 1] < id_bound)
emitted_type[in[i + 1]] = true;
i += wc;
continue;
}
uint32_t w[4];
memcpy(w, &in[i], sizeof(w));
uint32_t sampled_type = 0;
uint32_t sampled_image_type = 0;
for (size_t scan = 5; scan < in_c;)
{
uint32_t sw = in[scan] >> 16;
uint32_t sop = in[scan] & 0xffffu;
if (sw == 0 || scan + sw > in_c)
break;
if (sw >= 3 && in[scan + 2] == in[i + 3])
{
sampled_type = in[scan + 1];
break;
}
scan += sw;
}
if (sampled_type)
{
for (size_t scan = 5; scan < in_c;)
{
uint32_t sw = in[scan] >> 16;
uint32_t sop = in[scan] & 0xffffu;
if (sw == 0 || scan + sw > in_c)
break;
if (sop == SpvOpTypeSampledImage &&
sw >= 3 &&
in[scan + 1] == sampled_type)
{
sampled_image_type = in[scan + 2];
break;
}
scan += sw;
}
}
if (sampled_image_type)
{
for (uint32_t img = 0; img < s.n_img; ++img)
{
if (!s.images[img].replacement_type)
continue;
if (s.images[img].id != sampled_image_type &&
s.images[img].replacement_type != sampled_image_type)
continue;
if (s.images[img].replacement_type >= id_bound ||
!emitted_type[s.images[img].replacement_type])
{
continue;
}
w[1] = s.images[img].replacement_type;
break;
}
}
int load = fs_find_load(&s, in[i + 3]);
if (load < 0)
{
for (uint32_t ii = 0; ii < s.n_load; ++ii)
{
}
}
if (load >= 0)
{
uint32_t owner = s.loads[load].owner_var;
for (uint32_t img = 0; img < s.n_img; ++img)
{
if (s.images[img].dim != SpvDim2D)
continue;
if (s.images[img].id != w[1])
continue;
if (!s.images[img].replacement_type)
continue;
w[1] = s.images[img].replacement_type;
break;
}
for (uint32_t img = 0; img < s.n_img; ++img)
{
if (s.images[img].owner_var != owner)
continue;
if (s.images[img].dim != SpvDim2D)
continue;
if (s.images[img].replacement_type &&
s.images[img].replacement_type < id_bound &&
emitted_type[s.images[img].replacement_type])
{
w[1] = s.images[img].replacement_type;
break;
}
}
for (uint32_t img = 0; img < s.n_img; ++img)
{
if (s.images[img].owner_var != owner)
continue;
if (s.images[img].dim != SpvDim2D)
continue;
if (!s.images[img].replacement_type)
continue;
if (s.images[img].replacement_type >= id_bound ||
!emitted_type[s.images[img].replacement_type])
{
continue;
}
w[1] = s.images[img].replacement_type;
break;
}
if (w[1] == in[i + 1])
{
int source_var = fs_var_index(&s, s.loads[load].source_id);
int loaded_type = (source_var >= 0) ? s.vars[source_var].type : -1;
if (loaded_type >= 0)
{
for (uint32_t img = 0; img < s.n_img; ++img)
{
if (s.images[img].dim != SpvDim2D)
continue;
if (s.images[img].sampled_type_id != (uint32_t)loaded_type)
continue;
if (!s.images[img].replacement_type ||
s.images[img].replacement_type >= id_bound ||
!emitted_type[s.images[img].replacement_type])
continue;
w[1] = s.images[img].replacement_type;
break;
}
}
}
}
if (w[1] == in[i + 1])
{
uint32_t sampled_type = 0;
for (size_t scan = 5; scan < in_c;)
{
uint32_t sw = in[scan] >> 16;
uint32_t sop = in[scan] & 0xffffu;
if (sw == 0)
break;
if (sop == SpvOpLoad && sw >= 4 && in[scan + 2] == in[i + 3])
{
sampled_type = in[scan + 1];
break;
}
scan += sw;
}
if (sampled_type)
{
for (uint32_t img = 0; img < s.n_img; ++img)
{
if (s.images[img].dim != SpvDim2D)
continue;
if (s.images[img].sampled_type_id != sampled_type)
continue;
if (!s.images[img].replacement_type ||
s.images[img].replacement_type >= id_bound ||
!emitted_type[s.images[img].replacement_type])
continue;
w[1] = s.images[img].replacement_type;
break;
}
}
}
if (w[1] < id_bound && w[1] != in[i + 1] && !emitted_type[w[1]])
{
w[1] = in[i + 1];
}
sb_push_n(&ob, w, wc);
if (w[1] < id_bound)
{
emitted_type[w[1]] = true;
}
i += wc;
continue;
}
if (in_func &&
(op == SpvOpImageQuerySizeLod || op == SpvOpImageQuerySize) &&
wc >= 4)
{
}
if (in_func &&
(op == SpvOpImageQuerySizeLod || op == SpvOpImageQuerySize) &&
wc >= 4)
{
uint32_t image_ssa = in[i + 3];
uint32_t image_type = 0;
for (size_t scan = 5; scan < in_c;)
{
uint32_t sw = in[scan] >> 16;
uint32_t sop = in[scan] & 0xffffu;
if (!sw || scan + sw > in_c)
break;
if ((sop == SpvOpLoad && sw >= 4 && in[scan + 2] == image_ssa) || (sop == SpvOpImage && sw >= 4 && in[scan + 2] == image_ssa))
{
image_type = in[scan + 1];
break;
}
scan += sw;
}
int img_idx = -1;
for (uint32_t ii = 0; ii < s.n_img; ++ii)
{
if (s.images[ii].dim != SpvDim2D)
continue;
if (!s.images[ii].replacement_type)
continue;
if (s.images[ii].id != image_type &&
s.images[ii].replacement_type != image_type)
continue;
img_idx = (int)ii;
break;
}
if (img_idx < 0)
{
sb_push_n(&ob, &in[i], wc);
if (in[i + 1] < id_bound)
{
emitted_type[in[i + 1]] = true;
}
i += wc;
continue;
}
if (image_type != s.images[img_idx].replacement_type)
{
}
uint32_t *w = malloc(wc * sizeof(uint32_t));
if (!w)
{
sb_push_n(&ob, &in[i], wc);
if (in[i + 1] < id_bound)
emitted_type[in[i + 1]] = true;
i += wc;
continue;
}
memcpy(w, &in[i], wc * sizeof(uint32_t));
uint32_t old_result_type = w[1];
uint32_t old_result_id = w[2];
uint32_t result_component_type = 0;
for (size_t scan = 5; scan < in_c;)
{
uint32_t sw = in[scan] >> 16;
uint32_t sop = in[scan] & 0xffffu;
if (!sw || scan + sw > in_c)
break;
if (sop == SpvOpTypeVector && sw >= 4 && in[scan + 1] == old_result_type && in[scan + 3] == 2)
{
result_component_type = in[scan + 2];
break;
}
scan += sw;
}
uint32_t query_result_type = 0;
uint32_t component_signedness = UINT32_MAX;
if (result_component_type)
{
for (size_t scan = 5; scan < in_c;)
{
uint32_t sw = in[scan] >> 16;
uint32_t sop = in[scan] & 0xffffu;
if (!sw || scan + sw > in_c)
break;
if (sop == SpvOpTypeInt && sw >= 4 && in[scan + 1] == result_component_type)
{
component_signedness = in[scan + 3];
break;
}
scan += sw;
}
}
if (component_signedness == 0)
query_result_type = new_v3u_id;
else if (component_signedness == 1)
query_result_type = new_v3i_id;
if (!query_result_type)
{
sb_push_n(&ob, &in[i], wc);
if (old_result_type < id_bound)
{
emitted_type[old_result_type] = true;
}
free(w);
i += wc;
continue;
}
if (old_result_type == query_result_type)
{
sb_push_n(&ob, w, wc);
if (w[1] < id_bound)
{
emitted_type[w[1]] = true;
}
free(w);
i += wc;
continue;
}
uint32_t query_v3_id = samp_nid++;
w[1] = query_result_type;
w[2] = query_v3_id;
sb_push_n(&ob, w, wc);
uint32_t shuffle[] = {
(7u << 16) | SpvOpVectorShuffle,
old_result_type,
old_result_id,
query_v3_id,
query_v3_id,
0,
1
};
sb_push_n(&ob, shuffle, 7);
if (old_result_type < id_bound)
{
emitted_type[old_result_type] = true;
}
if (query_v3_id < id_bound)
{
emitted_type[query_v3_id] = true;
}
free(w);
i += wc;
continue;
}
if (in_func && op == SpvOpSampledImage && wc == 5)
{
uint32_t image_id = in[i+3];
uint32_t image_value_type = 0;
for (size_t scan = 5; scan < ob.n;)
{
uint32_t sw = ob.w[scan] >> 16;
uint32_t sop = ob.w[scan] & 0xffffu;
if (!sw || scan + sw > ob.n)
break;
if ((sop == SpvOpLoad || sop == SpvOpImage) && sw >= 4 && ob.w[scan+2] == image_id)
{
image_value_type = ob.w[scan+1];
break;
}
scan += sw;
}
uint32_t matching_sampled_type = 0;
if (image_value_type)
{
for (size_t scan = 5; scan < ob.n;)
{
uint32_t sw = ob.w[scan] >> 16;
uint32_t sop = ob.w[scan] & 0xffffu;
if (!sw || scan + sw > ob.n)
break;
if (sop == SpvOpTypeSampledImage && sw >= 3 && ob.w[scan+2] == image_value_type)
{
matching_sampled_type = ob.w[scan+1];
break;
}
scan += sw;
}
}
if (matching_sampled_type)
{
uint32_t w[5];
memcpy(w, &in[i], sizeof(w));
w[1] = matching_sampled_type;
sb_push_n(&ob, w, 5);
if (w[2] < id_bound)
emitted_type[w[2]] = true;
i += wc;
continue;
}
sb_push_n(&ob, &in[i], wc);
if (in[i+2] < id_bound)
emitted_type[in[i+2]] = true;
i += wc;
continue;
}
if (in_func && (op == SpvOpImageFetch || op == SpvOpImageRead) && wc >= 5)
{
uint32_t coord_id = in[i+4];
uint32_t descriptor_var = 0;
bool image_known = false;
int load =
fs_find_load(
&s,
in[i+3]);
if (load >= 0)
{
descriptor_var =
s.loads[load].owner_var;
image_known = true;
}
int vi = fs_var_index(&s, descriptor_var);
if (vi >= 0)
{
}
if (in[i+3] == 47)
{
}
bool image_type_rewritten = false;
uint32_t fetch_image_type = 0;
uint32_t fetch_def_opcode = 0;
uint32_t fetch_def_type = 0;
for (size_t scan = 5; scan < in_c;)
{
uint32_t sw = in[scan] >> 16;
uint32_t sop = in[scan] & 0xffffu;
if (sw == 0 || scan + sw > in_c)
break;
bool has_result_type =
(sop == SpvOpLoad && sw >= 4) ||
(sop == SpvOpImage && sw >= 4) ||
(sop == SpvOpSampledImage && sw >= 5);
if (has_result_type && in[scan + 2] == in[i + 3])
{
fetch_def_opcode = sop;
fetch_def_type = in[scan + 1];
break;
}
scan += sw;
}
if (fetch_def_type)
{
if (fetch_def_opcode == SpvOpLoad ||
fetch_def_opcode == SpvOpImage)
{
fetch_image_type = fetch_def_type;
}
else if (fetch_def_opcode == SpvOpSampledImage)
{
uint32_t sampled_image_type = fetch_def_type;
for (size_t scan = 5; scan < in_c;)
{
uint32_t sw = in[scan] >> 16;
uint32_t sop = in[scan] & 0xffffu;
if (sw == 0 || scan + sw > in_c)
break;
if (sop == SpvOpTypeSampledImage &&
sw >= 3 &&
in[scan + 1] == sampled_image_type)
{
fetch_image_type = in[scan + 2];
break;
}
scan += sw;
}
}
}
for (uint32_t img = 0; img < s.n_img; ++img)
{
if (!s.images[img].replacement_type)
continue;
if (s.images[img].id != fetch_image_type &&
s.images[img].replacement_type != fetch_image_type &&
s.images[img].sampled_type_id != fetch_image_type &&
s.images[img].replacement_sampled_type != fetch_image_type)
continue;
image_type_rewritten = true;
break;
}
bool fetch_stereo_attachment = fs_binding_is_stereo_attachment(&s, descriptor_var);
if (!fetch_stereo_attachment && !image_type_rewritten)
{
sb_push_n(&ob, &in[i], wc);
if (in[i + 1] < id_bound)
{
emitted_type[in[i + 1]] = true;
}
i += wc;
continue;
}
if (!image_type_rewritten)
{
sb_push_n(&ob, &in[i], wc);
if (in[i + 1] < id_bound)
{
emitted_type[in[i + 1]] = true;
}
i += wc;
continue;
}
fs_dump_descriptor_chain(
&s,
in,
in_c,
descriptor_var);
uint32_t id_lv = samp_nid++;
uint32_t id_x = samp_nid++;
uint32_t id_y = samp_nid++;
uint32_t id_c3 = samp_nid++;
{ uint32_t w[]={(4u<<16)|SpvOpLoad, new_vi_type, id_lv, new_vi_id};
sb_push_n(&ob,w,4); }
uint32_t id_layer = id_lv;
uint32_t coord_scalar_type = new_int_id;
uint32_t coord_vector_type = new_v3i_id;
uint32_t coord_type =
fs_result_type_of(&s, in, in_c, coord_id);
bool coord_is_uint =
coord_type &&
s.uint_id &&
(coord_type == s.v2uint_id ||
coord_type == s.v3uint_id);
if (coord_is_uint)
{
if (!new_v3u_id)
break;
coord_scalar_type = s.uint_id;
coord_vector_type = new_v3u_id;
if (new_vi_type != s.uint_id)
{
id_layer = samp_nid++;
uint32_t w[] = {
(4u << 16) | SpvOpBitcast,
s.uint_id,
id_layer,
id_lv
};
sb_push_n(&ob, w, 4);
}
}
else if (new_vi_type != new_int_id)
{
id_layer = samp_nid++;
uint32_t w[] = {
(4u << 16) | SpvOpBitcast,
new_int_id,
id_layer,
id_lv
};
sb_push_n(&ob, w, 4);
}
{ uint32_t w[]={(5u<<16)|SpvOpCompositeExtract, coord_scalar_type, id_x, coord_id, 0};
sb_push_n(&ob,w,5); }
{ uint32_t w[]={(5u<<16)|SpvOpCompositeExtract, coord_scalar_type, id_y, coord_id, 1};
sb_push_n(&ob,w,5); }
{ uint32_t w[]={(6u<<16)|SpvOpCompositeConstruct, coord_vector_type, id_c3,
id_x, id_y, id_layer};
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
if (op == SpvOpConstantComposite && wc >= 4)
{
bool has_spec_constant = false;
for (uint32_t operand = 3; operand < wc; ++operand)
{
uint32_t constituent = in[i + operand];
for (size_t scan = 5; scan < in_c;)
{
uint32_t sw = in[scan] >> 16;
uint32_t sop = in[scan] & 0xffffu;
if (sw == 0 || scan + sw > in_c)
break;
if (sw >= 3 && in[scan + 2] == constituent && (sop == SpvOpSpecConstantTrue || sop == SpvOpSpecConstantFalse || sop == SpvOpSpecConstant || sop == SpvOpSpecConstantComposite || sop == SpvOpSpecConstantOp))
{
has_spec_constant = true;
break;
}
scan += sw;
}
if (has_spec_constant)
break;
}
if (has_spec_constant)
{
uint32_t w = ((wc << 16) | SpvOpSpecConstantComposite);
sb_push(&ob, w);
sb_push_n(&ob, &in[i + 1], wc - 1);
i += wc;
continue;
}
}
if (op >= SpvOpImageSampleImplicitLod &&
op <= SpvOpImageSampleProjDrefExplicitLod)
{
}
sb_push_n(&ob, &in[i], wc);
if (in[i + 1] < id_bound)
{
emitted_type[in[i + 1]] = true;
}
i += wc;
}
if (nid > samp_nid)
samp_nid = nid;
ob.w[3] = samp_nid;
*out = ob.w;
*out_c = ob.n;
return true;
}
static bool
spirv_patch_stereo_raygen(
const uint32_t *in,
size_t in_c,
uint32_t **out,
size_t *out_c,
float lo,
float ro,
float conv,
int projection_mode)
{
if (!in || in_c < 5 || in[0] != SPIRV_MAGIC || !out || !out_c)
return false;
*out = NULL;
*out_c = 0;
uint32_t bound = in[3];
uint32_t launch_id_var = 0;
uint32_t launch_id_load = 0;
uint32_t image_type = 0;
uint32_t image_read_coord = 0;
uint32_t image_write_coord = 0;
uint32_t float_zero = 0;
uint32_t int_type = 0;
uint32_t uint_type = 0;
uint32_t float_type = 0;
uint32_t bool_type = 0;
uint32_t v2int_type = 0;
uint32_t v3uint_type = 0;
uint32_t v4float_type = 0;
uint32_t v3int_type = 0;
uint32_t first_function = 0;
uint32_t image_texel_type = 0;
uint32_t origin_vec = 0;
uint32_t origin_mat = 0;
uint32_t origin_mtv = 0;
uint32_t origin_result = 0;
uint32_t origin_var = 0;
uint32_t ray_ndc_vec = 0;
uint32_t ray_ndc_mat = 0;
uint32_t ray_ndc_mtv = 0;
uint32_t matrix_times_vector_count = 0;
uint32_t first_label = 0;
for (size_t i = 5; i < in_c;)
{
uint32_t op = in[i] & 0xffffu;
uint32_t wc = in[i] >> 16;
if (!wc || i + wc > in_c)
{
return false;
}
if (in[i + 1] >= bound && in[i + 1] <= bound + 32)
if (wc >= 3 && in[i + 2] >= bound && in[i + 2] <= bound + 32)
if (op == SpvOpDecorate &&
wc >= 4 &&
in[i + 2] == SpvDecorationBuiltIn &&
in[i + 3] == SpvBuiltInLaunchIdKHR)
{
launch_id_var = in[i + 1];
}
else if (op == SpvOpTypeInt && wc >= 4)
{
if (in[i + 2] == 32 && in[i + 3] == 0)
uint_type = in[i + 1];
else if (in[i + 2] == 32 && in[i + 3] == 1)
int_type = in[i + 1];
}
else if (op == SpvOpTypeFloat && wc >= 3)
{
if (in[i + 2] == 32)
float_type = in[i + 1];
}
else if (op == SpvOpTypeBool && wc >= 2)
{
bool_type = in[i + 1];
}
else if (op == SpvOpTypeVector && wc >= 4)
{
if (in[i + 2] == int_type && in[i + 3] == 2)
v2int_type = in[i + 1];
if (in[i + 2] == int_type && in[i + 3] == 3 && !v3int_type)
v3int_type = in[i + 1];
if (in[i + 2] == uint_type && in[i + 3] == 3)
v3uint_type = in[i + 1];
if (in[i + 2] == float_type && in[i + 3] == 4)
v4float_type = in[i + 1];
}
else if (op == SpvOpTypeImage &&
wc >= 9 &&
in[i + 3] == SpvDim2D &&
in[i + 4] == 0 &&
in[i + 5] == 0 &&
in[i + 6] == 0 &&
in[i + 7] == 2)
{
image_type = in[i + 1];
image_texel_type = in[i + 2];
}
else if (op == SpvOpConstant &&
wc >= 4 &&
in[i + 1] == float_type &&
in[i + 3] == 0 &&
!float_zero)
{
float_zero = in[i + 2];
}
else if (op == SpvOpLoad &&
wc >= 4 &&
launch_id_var &&
in[i + 3] == launch_id_var)
{
launch_id_load = in[i + 2];
}
else if (op == SpvOpImageRead && wc >= 5)
{
if (!image_read_coord)
image_read_coord = in[i + 4];
}
else if (op == SpvOpImageWrite && wc >= 4)
{
if (!image_write_coord)
image_write_coord = in[i + 2];
}
else if (op == SpvOpMatrixTimesVector && wc >= 5)
{
matrix_times_vector_count++;
if (matrix_times_vector_count == 1)
{
origin_mtv = (uint32_t)i;
origin_mat = in[i + 3];
origin_vec = in[i + 4];
origin_result = in[i + 2];
}
else if (matrix_times_vector_count == 2)
{
ray_ndc_mtv = (uint32_t)i;
ray_ndc_mat = in[i + 3];
ray_ndc_vec = in[i + 4];
}
}
else if (op == SpvOpStore &&
wc >= 3 &&
origin_result &&
in[i + 2] == origin_result &&
!origin_var)
{
origin_var = in[i + 1];
}
else if (op == SpvOpFunction && !first_function)
{
first_function = (uint32_t)i;
}
else if (op == SpvOpLabel && first_function && !first_label)
{
first_label = (uint32_t)i;
}
i += wc;
}
if (!launch_id_var ||
!launch_id_load ||
!image_type ||
(!image_read_coord && !image_write_coord) ||
!first_function ||
!first_label ||
!int_type ||
!uint_type ||
!float_type ||
!v2int_type ||
!v3uint_type ||
!v4float_type ||
!image_texel_type ||
!float_zero ||
!origin_mtv ||
!origin_vec ||
!origin_var ||
!ray_ndc_mtv ||
!ray_ndc_vec)
{
return false;
}
for (size_t d = 5; d < in_c;)
{
uint32_t dop = in[d] & 0xffffu;
uint32_t dwc = in[d] >> 16;
if (!dwc || d + dwc > in_c)
break;
if (dop == SpvOpTraceRayKHR || dop == SpvOpImageWrite || dop == SpvOpMatrixTimesVector)
d += dwc;
}
uint32_t generated_id_base = bound;
uint32_t generated_id = generated_id_base;
uint32_t stereo_launch_load = generated_id++;
if (!v3int_type)
v3int_type = generated_id++;
uint32_t new_bool_type = bool_type;
if (!new_bool_type)
new_bool_type = generated_id++;
uint32_t coord_x = generated_id++;
uint32_t coord_y = generated_id++;
uint32_t coord_z_u = generated_id++;
uint32_t coord_z = generated_id++;
uint32_t new_coord = generated_id++;
uint32_t eye_is_left = generated_id++;
uint32_t selected_offset = generated_id++;
uint32_t left_const = generated_id++;
uint32_t right_const = generated_id++;
uint32_t conv_const = generated_id++;
uint32_t uint_zero_const = generated_id++;
uint32_t ray_ndc_raw = generated_id++;
uint32_t inv_p00 = generated_id++;
uint32_t eye_product = generated_id++;
uint32_t eye_shift = generated_id++;
uint32_t eye_origin = generated_id++;
uint32_t new_origin = generated_id++;
uint32_t ray_x = generated_id++;
uint32_t ray_y = generated_id++;
uint32_t ray_z = generated_id++;
uint32_t ray_w = generated_id++;
uint32_t ray_conv = generated_id++;
uint32_t ray_new_x = generated_id++;
uint32_t coord_x_int = generated_id++;
uint32_t coord_y_int = generated_id++;
bound = generated_id;
uint32_t type_vec3[] = {
(4u << 16) | SpvOpTypeVector,
v3int_type,
int_type,
3
};
uint32_t c_left[] = {
(4u << 16) | SpvOpConstant,
float_type,
left_const,
0
};
uint32_t c_right[] = {
(4u << 16) | SpvOpConstant,
float_type,
right_const,
0
};
uint32_t c_conv[] = {
(4u << 16) | SpvOpConstant,
float_type,
conv_const,
0
};
uint32_t c_uint_zero[] = {
(4u << 16) | SpvOpConstant,
uint_type,
uint_zero_const,
0
};
uint32_t launch_z[] = {
(5u << 16) | SpvOpCompositeExtract,
uint_type,
coord_z_u,
stereo_launch_load,
2
};
uint32_t left_test[] = {
(5u << 16) | SpvOpIEqual,
new_bool_type,
eye_is_left,
coord_z_u,
uint_zero_const
};
uint32_t select[] = {
(6u << 16) | SpvOpSelect,
float_type,
selected_offset,
eye_is_left,
right_const,
left_const
};
uint32_t stereo_launch[] = {
(4u << 16) | SpvOpLoad,
v3uint_type,
stereo_launch_load,
launch_id_var
};
float f_left = lo;
float f_right = ro;
float f_conv = conv;
memcpy(&c_left[3], &f_left, sizeof(f_left));
memcpy(&c_right[3], &f_right, sizeof(f_right));
memcpy(&c_conv[3], &f_conv, sizeof(f_conv));
SpvBuf ob;
sb_init(&ob, in_c + 256);
sb_push_n(&ob, in, 5);
size_t first_function_pos = first_function;
size_t stereo_insert_pos = 0;
for (size_t s = first_label; s < in_c;)
{
uint32_t sop = in[s] & 0xffffu;
uint32_t swc = in[s] >> 16;
if (!swc || s + swc > in_c)
{
sb_free(&ob);
return false;
}
if (s == first_label)
{
s += swc;
continue;
}
if (sop != SpvOpVariable)
{
stereo_insert_pos = s;
break;
}
s += swc;
}
if (!stereo_insert_pos)
{
sb_free(&ob);
return false;
}
for (size_t i = 5; i < in_c;)
{
if (i == first_function_pos)
{
if (v3int_type >= generated_id_base)
sb_push_n(&ob, type_vec3, 4);
if (!bool_type)
{
uint32_t type_bool[] = {
(2u << 16) | SpvOpTypeBool,
new_bool_type
};
sb_push_n(&ob, type_bool, 2);
}
sb_push_n(&ob, c_left, 4);
sb_push_n(&ob, c_right, 4);
sb_push_n(&ob, c_conv, 4);
sb_push_n(&ob, c_uint_zero, 4);
}
uint32_t op = in[i] & 0xffffu;
uint32_t wc = in[i] >> 16;
if (!wc || i + wc > in_c)
{
sb_free(&ob);
return false;
}
if (i == stereo_insert_pos)
{
sb_push_n(&ob, stereo_launch, 4);
sb_push_n(&ob, launch_z, 5);
sb_push_n(&ob, left_test, 5);
sb_push_n(&ob, select, 6);
uint32_t coord_x_inst[] = {
(5u << 16) | SpvOpCompositeExtract,
uint_type,
coord_x,
stereo_launch_load,
0
};
uint32_t coord_y_inst[] = {
(5u << 16) | SpvOpCompositeExtract,
uint_type,
coord_y,
stereo_launch_load,
1
};
uint32_t coord_z_cast[] = {
(4u << 16) | SpvOpBitcast,
int_type,
coord_z,
coord_z_u
};
uint32_t coord_x_cast[] = {
(4u << 16) | SpvOpBitcast,
int_type,
coord_x_int,
coord_x
};
uint32_t coord_y_cast[] = {
(4u << 16) | SpvOpBitcast,
int_type,
coord_y_int,
coord_y
};
uint32_t coord_construct[] = {
(6u << 16) | SpvOpCompositeConstruct,
v3int_type,
new_coord,
coord_x_int,
coord_y_int,
coord_z
};
sb_push_n(&ob, coord_x_inst, 5);
sb_push_n(&ob, coord_y_inst, 5);
sb_push_n(&ob, coord_x_cast, 4);
sb_push_n(&ob, coord_y_cast, 4);
sb_push_n(&ob, coord_z_cast, 4);
sb_push_n(&ob, coord_construct, 6);
}
if (op == SpvOpTypeImage &&
wc >= 9 &&
in[i + 1] == image_type &&
in[i + 5] == 0)
{
sb_push_n(&ob, &in[i], wc);
ob.w[ob.n - wc + 5] = 1;
i += wc;
continue;
}
if (op == SpvOpMatrixTimesVector &&
i == origin_mtv)
{
sb_push_n(&ob, &in[i], wc);
i += wc;
continue;
}
if (op == SpvOpMatrixTimesVector &&
i == ray_ndc_mtv)
{
uint32_t result_id = in[i + 2];
uint32_t raw_mtv[5];
memcpy(raw_mtv, &in[i], sizeof(raw_mtv));
raw_mtv[2] = ray_ndc_raw;
sb_push_n(&ob, raw_mtv, 5);
uint32_t p00_inv[] = {
(6u << 16) | SpvOpCompositeExtract,
float_type,
inv_p00,
ray_ndc_mat,
0,
0
};
uint32_t eye_mul[] = {
(5u << 16) | SpvOpFMul,
float_type,
eye_product,
selected_offset,
inv_p00
};
uint32_t eye_neg[] = {
(4u << 16) | SpvOpFNegate,
float_type,
eye_shift,
eye_product
};
uint32_t eye_vec[] = {
(6u << 16) | SpvOpCompositeInsert,
v4float_type,
eye_origin,
eye_shift,
origin_vec,
0
};
uint32_t eye_mtv[] = {
(5u << 16) | SpvOpMatrixTimesVector,
v4float_type,
new_origin,
origin_mat,
eye_origin
};
uint32_t store_origin[] = {
(3u << 16) | SpvOpStore,
origin_var,
new_origin
};
sb_push_n(&ob, p00_inv, 6);
sb_push_n(&ob, eye_mul, 5);
sb_push_n(&ob, eye_neg, 4);
sb_push_n(&ob, eye_vec, 6);
sb_push_n(&ob, eye_mtv, 5);
sb_push_n(&ob, store_origin, 3);
uint32_t x[] = {
(5u << 16) | SpvOpCompositeExtract,
float_type,
ray_x,
ray_ndc_raw,
0
};
uint32_t y[] = {
(5u << 16) | SpvOpCompositeExtract,
float_type,
ray_y,
ray_ndc_raw,
1
};
uint32_t z[] = {
(5u << 16) | SpvOpCompositeExtract,
float_type,
ray_z,
ray_ndc_raw,
2
};
uint32_t w[] = {
(5u << 16) | SpvOpCompositeExtract,
float_type,
ray_w,
ray_ndc_raw,
3
};
sb_push_n(&ob, x, 5);
sb_push_n(&ob, y, 5);
sb_push_n(&ob, z, 5);
sb_push_n(&ob, w, 5);
uint32_t x_conv[] = {
(5u << 16) | SpvOpFMul,
float_type,
ray_conv,
selected_offset,
conv_const
};
uint32_t x_add[] = {
(5u << 16) | SpvOpFAdd,
float_type,
ray_new_x,
ray_x,
ray_conv
};
sb_push_n(&ob, x_conv, 5);
sb_push_n(&ob, x_add, 5);
uint32_t construct[] = {
(7u << 16) | SpvOpCompositeConstruct,
v4float_type,
result_id,
ray_new_x,
ray_y,
ray_z,
ray_w
};
sb_push_n(&ob, construct, 7);
i += wc;
continue;
}
if (op == SpvOpImageRead &&
wc >= 5)
{
sb_push_n(&ob, &in[i], wc);
ob.w[ob.n - wc + 4] = new_coord;
i += wc;
continue;
}
if (op == SpvOpImageWrite &&
wc >= 4)
{
sb_push_n(&ob, &in[i], wc);
ob.w[ob.n - wc + 2] = new_coord;
i += wc;
continue;
}
sb_push_n(&ob, &in[i], wc);
i += wc;
}
ob.w[3] = bound;
*out = ob.w;
*out_c = ob.n;
return true;
}
static bool is_patchable_spv(const uint32_t *w, size_t c)
{
if (c<5||w[0]!=SPIRV_MAGIC) return false;
for (size_t i=5;i<c;) {
uint32_t op=w[i]&0xffff, wc=w[i]>>16; if (!wc||i+wc>c) break;
if (op==SpvOpEntryPoint&&wc>=2) {
uint32_t e=w[i+1];
return e==SpvExecVertex||e==SpvExecGeometry||e==SpvExecTessEval||e==SpvExecFragment;
}
i+=wc;
}
return false;
}
static void log_rt_raygen_spv(const uint32_t *w, size_t c)
{
if (!w || c < 5 || w[0] != SPIRV_MAGIC)
return;
for (size_t i = 5; i < c;)
{
uint32_t op = w[i] & 0xffffu;
uint32_t wc = w[i] >> 16;
if (!wc || i + wc > c)
break;
if (op == SpvOpEntryPoint && wc >= 3)
{
}
else if (op == SpvOpDecorate && wc >= 4)
{
}
else if (op == SpvOpTypeImage && wc >= 9)
{
}
else if (op == SpvOpVariable && wc >= 4)
{
}
else if (op == SpvOpImageWrite && wc >= 4)
{
}
else if (op == SpvOpLoad && wc >= 4)
{
}
i += wc;
}
}
static bool is_rt_raygen_spv(const uint32_t *w, size_t c)
{
if (c < 5 || w[0] != SPIRV_MAGIC)
return false;
for (size_t i = 5; i < c;)
{
uint32_t op = w[i] & 0xffffu;
uint32_t wc = w[i] >> 16;
if (!wc || i + wc > c)
break;
if (op == SpvOpEntryPoint && wc >= 2)
return w[i + 1] == SpvExecutionModelRayGenerationKHR;
i += wc;
}
return false;
}
static StereoShaderCache *
cache_find(StereoDevice *sd, VkShaderModule mod)
{
if (!sd)
return NULL;
for (uint32_t i = 0; i < sd->shader_cache_count; i++)
{
StereoShaderCache *e = &sd->shader_cache[i];
if (e->handle == mod)
{
return e;
}
}
return NULL;
}
static void cache_add(StereoDevice *sd, VkShaderModule h,
const uint32_t *spv, size_t words) {
if (sd->shader_cache_count>=MAX_SHADER_CACHE) return;
uint32_t *cp=malloc(words*4); if (!cp) return;
memcpy(cp,spv,words*4);
CHECK_ARRAY_COUNT(sd->shader_cache_count, MAX_SHADER_CACHE, "shader_cache_count");
StereoShaderCache *e=&sd->shader_cache[sd->shader_cache_count++];
e->handle=h; e->spv=cp; e->words=words; e->exec_model=-1;
for (size_t i=5;i<words;) {
uint32_t wc=spv[i]>>16;
uint32_t op=spv[i]&0xffff;
if (!wc || i+wc>words) break;
if (op==SpvOpEntryPoint && wc>=3) {
e->exec_model=(int)spv[i+1];
break;
}
i+=wc;
}
}
static void cache_remove(StereoDevice *sd, VkShaderModule h)
{
for (uint32_t i = 0; i < sd->shader_cache_count; i++)
{
if (sd->shader_cache[i].handle == h)
{
free(sd->shader_cache[i].spv);
uint32_t last = --sd->shader_cache_count;
if (i != last)
sd->shader_cache[i] = sd->shader_cache[last];
memset(&sd->shader_cache[last], 0,
sizeof(sd->shader_cache[last]));
return;
}
}
}
VKAPI_ATTR VkResult VKAPI_CALL
stereo_CreateShaderModule(VkDevice device, const VkShaderModuleCreateInfo *pCI,
const VkAllocationCallbacks *pAlloc, VkShaderModule *pSM)
{
StereoDevice *sd=stereo_device_from_handle(device);
if (!sd) return VK_ERROR_DEVICE_LOST;
VkResult res=sd->real.CreateShaderModule(sd->real_device,pCI,pAlloc,pSM);
if (res!=VK_SUCCESS) return res;
if (!sd->stereo.enabled) return VK_SUCCESS;
const uint32_t *spv = (const uint32_t *)pCI->pCode;
size_t wc = pCI->codeSize / 4;
uint64_t h = hash_spv(spv, wc);
const char *dump = stereo_getenv("VKS3D_DUMP_SPIRV");
if (dump)
{
char dp[512];
_snprintf(
dp,
sizeof(dp) - 1,
"%s\\%016llx.spv",
dump,
(unsigned long long)h);
FILE *f = fopen(dp, "rb");
if (!f)
{
f = fopen(dp, "wb");
if (f)
{
fwrite(
spv,
4,
wc,
f);
fclose(f);
}
}
else
{
fclose(f);
}
}
int create_exec_model=-1;
for (size_t i=5;i<wc;) {
uint32_t iw=spv[i]>>16;
uint32_t io=spv[i]&0xffff;
if (!iw || i+iw>wc) break;
if (io==SpvOpEntryPoint && iw>=3) {
create_exec_model=(int)spv[i+1];
break;
}
i+=iw;
}
bool create_is_mesh =
create_exec_model == SpvExecMeshEXT;
bool create_is_patchable =
is_patchable_spv(spv,wc);
bool create_is_rt_raygen =
is_rt_raygen_spv(spv,wc);
if (create_is_rt_raygen)
log_rt_raygen_spv(spv, wc);
if (create_is_patchable || create_is_mesh || create_is_rt_raygen)
{
cache_add(sd,*pSM,spv,wc);
}
return VK_SUCCESS;
}
static const VkShaderModuleCreateInfo *stereo_stage_inline_spv(const VkPipelineShaderStageCreateInfo *stage)
{
const VkBaseInStructure *x=(const VkBaseInStructure *)stage->pNext;
while(x)
{
if(x->sType==VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO)
return (const VkShaderModuleCreateInfo *)x;
x=x->pNext;
}
return NULL;
}
VKAPI_ATTR VkResult VKAPI_CALL
stereo_CreateGraphicsPipelines(VkDevice device, VkPipelineCache pc,
uint32_t N, const VkGraphicsPipelineCreateInfo *pCI,
const VkAllocationCallbacks *pAlloc, VkPipeline *pP)
{
StereoDevice *sd=stereo_device_from_handle(device);
if (!sd) return VK_ERROR_DEVICE_LOST;
if (!sd->stereo.enabled)
return sd->real.CreateGraphicsPipelines(sd->real_device,pc,N,pCI,pAlloc,pP);
VkShaderModule *tmp_mod = calloc(N, sizeof(VkShaderModule));
VkPipelineShaderStageCreateInfo **tst = calloc(N, sizeof(void*));
VkGraphicsPipelineCreateInfo *infos = malloc(N * sizeof(*infos));
StereoDebugCtx *dbg_out = calloc(N, sizeof(*dbg_out));
for (uint32_t i = 0; i < N; i++)
{
dbg_out[i].proj_set = UINT32_MAX;
dbg_out[i].proj_binding = UINT32_MAX;
dbg_out[i].proj_member_mask = UINT32_MAX;
dbg_out[i].proj_var = UINT32_MAX;
}
if (!tmp_mod||!tst||!infos) {
free(tmp_mod); free(tst); free(infos);
return VK_ERROR_OUT_OF_HOST_MEMORY;
}
memcpy(infos, pCI, N * sizeof(*infos));
for (uint32_t p = 0; p < N; p++) {
}
const char *dump = stereo_getenv("VKS3D_DUMP_SPIRV");
static int dump_n = 0;
float lo=sd->stereo.left_eye_offset, ro=sd->stereo.right_eye_offset,
conv=sd->stereo.convergence;
for (uint32_t p=0; p<N; p++) {
const VkGraphicsPipelineCreateInfo *ci=&pCI[p];
const VkBaseInStructure *base =
(const VkBaseInStructure*)ci->pNext;
uint32_t gpl_flags = 0;
const VkBaseInStructure *gx=(const VkBaseInStructure *)ci->pNext;
while (gx)
{
if (gx->sType==VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_LIBRARY_CREATE_INFO_EXT)
gpl_flags=((const VkGraphicsPipelineLibraryCreateInfoEXT *)gx)->flags;
gx=gx->pNext;
}
uint32_t view_mask = 0;
while (base)
{
if (base->sType ==
VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO)
{
const VkPipelineRenderingCreateInfo *ri =
(const VkPipelineRenderingCreateInfo*)base;
VkPipelineRenderingCreateInfo *rw =
(VkPipelineRenderingCreateInfo*)base;
if (sd->stereo.multiview && rw->viewMask == 0) {
rw->viewMask = 0x3;
}
view_mask = rw->viewMask;
}
base = base->pNext;
}
if (!ci || ci->stageCount == 0 || !ci->pStages) {
}
if (!ci ||
ci->stageCount == 0 ||
!ci->pStages)
{
continue;
}
bool has_vs=false, has_tcs=false, has_tes=false, has_gs=false, has_ms=false, has_fs=false;
uint32_t vs_stage=~0u, tes_stage=~0u, gs_stage=~0u, ms_stage=~0u, fs_stage=~0u;
for (uint32_t s=0;s<ci->stageCount;s++) {
VkShaderStageFlagBits st=ci->pStages[s].stage;
if (st==VK_SHADER_STAGE_VERTEX_BIT)
{ has_vs=true; vs_stage=s; }
if (st==VK_SHADER_STAGE_FRAGMENT_BIT)
{ has_fs=true; fs_stage=s; }
if (st==VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT)
has_tcs=true;
if (st==VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT)
{ has_tes=true; tes_stage=s; }
if (st==VK_SHADER_STAGE_GEOMETRY_BIT)
{ has_gs=true; gs_stage=s; }
if (st==VK_SHADER_STAGE_MESH_BIT_EXT)
{ has_ms=true; ms_stage=s; }
}
StereoRenderPassInfo *rpi = NULL;
bool in_mv_rp = false;
VkRenderPass pipeline_rp = ci->renderPass;
if (ci->renderPass != VK_NULL_HANDLE) {
rpi = stereo_rp_lookup(sd, ci->renderPass);
in_mv_rp =
(rpi && rpi->has_multiview) ||
((view_mask & 0x3) != 0);
}
else if (sd->stereo.multiview && (view_mask & 0x3) != 0) {
in_mv_rp = true;
}
if (in_mv_rp &&
rpi &&
rpi->has_multiview &&
rpi->mv_handle != VK_NULL_HANDLE)
{
pipeline_rp = rpi->mv_handle;
}
else if (in_mv_rp)
{
}
for (uint32_t s2 = 0; s2 < ci->stageCount; s2++)
{
StereoShaderCache *sc =
cache_find(sd, ci->pStages[s2].module);
}
uint32_t gs_input_count=0;
uint64_t gs_hash=0;
uint32_t gs_output_count=0;
uint32_t gs_input_builtin_count=0;
uint32_t gs_output_builtin_count=0;
uint32_t gs_input_mode=~0u;
uint32_t gs_output_mode=~0u;
uint32_t gs_output_vertices=0;
if(has_gs&&gs_stage!=~0u)
{
StereoShaderCache *gs_cache=cache_find(sd,ci->pStages[gs_stage].module);
if(gs_cache)
{
SpvMod gs={0};
gs.words=gs_cache->spv;
gs.count=gs_cache->words;
if(has_gs&&gs_stage!=~0u)
{
StereoShaderCache *gs_cache=cache_find(sd,ci->pStages[gs_stage].module);
if(gs_cache&&gs_cache->spv&&gs_cache->words>=5)
{
SpvMod gs={0};
gs.words=gs_cache->spv;
gs.count=gs_cache->words;
gs.bound=gs.words[3];
gs.value_capacity=gs.bound+64;
gs.value_from_matrix=calloc(gs.value_capacity,sizeof(uint8_t));
gs.is_matrix_type=calloc(gs.value_capacity,sizeof(uint8_t));
gs.is_matrix_ptr=calloc(gs.value_capacity,sizeof(uint8_t));
gs.is_proj_value=calloc(gs.value_capacity,sizeof(uint8_t));
gs.is_view_value=calloc(gs.value_capacity,sizeof(uint8_t));
if(gs.value_from_matrix&&gs.is_matrix_type&&gs.is_matrix_ptr&&gs.is_proj_value&&gs.is_view_value)
{
spv_scan(&gs);
gs_hash=hash_spv(gs.words,gs.count);
uint32_t gs_entry_id=0;
for(uint32_t gi=5;gi<gs.count;)
{
uint32_t gwc=gs.words[gi]>>16;
uint32_t gop=gs.words[gi]&0xffffu;
if(!gwc||gwc>gs.count-gi)break;
if(gop==SpvOpEntryPoint&&gwc>=4&&gs.words[gi+1]==SpvExecutionModelGeometry)
{
gs_entry_id=gs.words[gi+2];
break;
}
gi+=gwc;
}
for(uint32_t gi=5;gi<gs.count;)
{
uint32_t gwc=gs.words[gi]>>16;
uint32_t gop=gs.words[gi]&0xffffu;
if(!gwc||gwc>gs.count-gi)break;
if(gop==SpvOpExecutionMode&&gwc>=3&&gs.words[gi+1]==gs_entry_id)
{
uint32_t mode=gs.words[gi+2];
if(mode==SpvExecutionModeInputPoints||mode==SpvExecutionModeInputLines||mode==SpvExecutionModeInputLinesAdjacency||mode==SpvExecutionModeTriangles||mode==SpvExecutionModeInputTrianglesAdjacency)gs_input_mode=mode;
else if(mode==SpvExecutionModeOutputPoints||mode==SpvExecutionModeOutputLineStrip||mode==SpvExecutionModeOutputTriangleStrip)gs_output_mode=mode;
else if(mode==SpvExecutionModeOutputVertices&&gwc>=4)gs_output_vertices=gs.words[gi+3];
}
gi+=gwc;
}
for(uint32_t gi=5;gi<gs.count;)
{
uint32_t gwc=gs.words[gi]>>16;
uint32_t gop=gs.words[gi]&0xffffu;
if(!gwc||gwc>gs.count-gi)break;
if(gop==SpvOpVariable&&gwc>=4)
{
uint32_t gid=gs.words[gi+2];
uint32_t storage=gs.words[gi+3];
if(storage==SpvStorageClassInput||storage==SpvStorageClassOutput)
{
bool has_location=false;
bool has_builtin=false;
for(uint32_t di=5;di<gs.count;)
{
uint32_t dwc=gs.words[di]>>16;
uint32_t dop=gs.words[di]&0xffffu;
if(!dwc||dwc>gs.count-di)break;
if(dop==SpvOpDecorate&&dwc>=4&&gs.words[di+1]==gid)
{
if(gs.words[di+2]==SpvDecorationLocation)has_location=true;
if(gs.words[di+2]==SpvDecorationBuiltIn)has_builtin=true;
}
di+=dwc;
}
if(has_location)
{
if(storage==SpvStorageClassInput)gs_input_count++;
else gs_output_count++;
}
if(has_builtin)
{
if(storage==SpvStorageClassInput)gs_input_builtin_count++;
else gs_output_builtin_count++;
}
}
}
gi+=gwc;
}
}
free_spv_provenance(&gs);
}
}
}
}
for (uint32_t fs_dbg_i = 0; fs_dbg_i < ci->stageCount; fs_dbg_i++) {
if (ci->pStages[fs_dbg_i].stage == VK_SHADER_STAGE_FRAGMENT_BIT) {
StereoShaderCache *fs_dbg =
cache_find(sd, ci->pStages[fs_dbg_i].module);
if (fs_dbg) {
}
}
}
if (!in_mv_rp)
{
continue;
}
bool is_quad = !ci->pVertexInputState ||
ci->pVertexInputState->vertexBindingDescriptionCount == 0;
bool vs_fullscreen = false;
bool vs_quad_fs = false;
bool vs_screen_space = false;
bool vs_background = false;
bool vs_procedural_sky = false;
bool vs_pure_quad = false;
bool vs_uv_quad = false;
bool vs_screen_quad = false;
if (has_vs && vs_stage != ~0u) {
StereoShaderCache *vs_cache = cache_find(sd,ci->pStages[vs_stage].module);
if (vs_cache) {
SpvMod vm = {0};
vm.words = vs_cache->spv;
vm.count = vs_cache->words;
vm.bound = vm.words[3];
vm.value_capacity = vm.bound + 64;
vm.value_from_matrix = calloc(vm.value_capacity,sizeof(uint8_t));
vm.is_matrix_type = calloc(vm.value_capacity,sizeof(uint8_t));
vm.is_matrix_ptr = calloc(vm.value_capacity,sizeof(uint8_t));
vm.is_proj_value = calloc(vm.value_capacity,sizeof(uint8_t));
vm.is_view_value = calloc(vm.value_capacity,sizeof(uint8_t));
if (vm.value_from_matrix && vm.is_matrix_type && vm.is_matrix_ptr && vm.is_proj_value && vm.is_view_value) {
spv_scan(&vm);
bool vs_has_user_output = false;
bool vs_has_v3_user_output = false;
uint32_t vs_location_id = 0;
for (size_t vi = 5; vi < vs_cache->words;)
{
uint32_t iw = vs_cache->spv[vi] >> 16;
uint32_t io = vs_cache->spv[vi] & 0xffff;
if (!iw || vi + iw > vs_cache->words)
break;
if (io == SpvOpDecorate && iw >= 4 && vs_cache->spv[vi + 2] == SpvDecorationLocation)
{
vs_has_user_output = true;
vs_location_id = vs_cache->spv[vi + 1];
}
vi += iw;
}
if (vs_location_id)
{
for (size_t vi = 5; vi < vs_cache->words;)
{
uint32_t iw = vs_cache->spv[vi] >> 16;
uint32_t io = vs_cache->spv[vi] & 0xffff;
if (!iw || vi + iw > vs_cache->words)
break;
if (io == SpvOpVariable && iw >= 4)
{
uint32_t var_type = vs_cache->spv[vi + 1];
uint32_t var_id = vs_cache->spv[vi + 2];
uint32_t storage = vs_cache->spv[vi + 3];
if (var_id == vs_location_id)
{
for (size_t ti = 5; ti < vs_cache->words;)
{
uint32_t tw = vs_cache->spv[ti] >> 16;
uint32_t to = vs_cache->spv[ti] & 0xffff;
if (!tw || ti + tw > vs_cache->words)
break;
if (to == SpvOpTypePointer && tw >= 4)
{
uint32_t pointer_id = vs_cache->spv[ti + 1];
uint32_t pointer_storage = vs_cache->spv[ti + 2];
uint32_t pointee_id = vs_cache->spv[ti + 3];
if (pointer_id == var_type)
if (pointer_id == var_type && pointer_storage == 3)
{
for (size_t vi2 = 5; vi2 < vs_cache->words;)
{
uint32_t iw2 = vs_cache->spv[vi2] >> 16;
uint32_t io2 = vs_cache->spv[vi2] & 0xffff;
if (!iw2 || vi2 + iw2 > vs_cache->words)
break;
if (io2 == SpvOpTypeVector && iw2 >= 4)
{
uint32_t vector_id = vs_cache->spv[vi2 + 1];
uint32_t component_id = vs_cache->spv[vi2 + 2];
uint32_t component_count = vs_cache->spv[vi2 + 3];
if (vector_id == pointee_id)
if (vector_id == pointee_id && component_count == 3)
{
vs_has_v3_user_output = true;
break;
}
}
vi2 += iw2;
}
break;
}
}
ti += tw;
}
}
}
vi += iw;
}
}
vs_quad_fs = !vm.has_matrix_ops && !vm.has_direct_position_write;
vs_screen_quad = is_quad && vm.exec_model == SpvExecVertex && !vm.has_matrix_ops && !vm.proj_found && !vm.has_emit_vertex;
vs_fullscreen = !vm.has_matrix_ops && !vm.has_direct_position_write && vm.has_v2_position_input;
vs_screen_space = vm.pos_is_block && !vm.has_matrix_ops && vm.has_v2_position_input && !vm.has_emit_vertex && vm.exec_model == SpvExecVertex;
bool vs_z_one_position = false;
uint32_t vs_position_ptr = 0;
uint32_t vs_position_value = 0;
uint32_t vs_position_type = 0;
uint32_t vs_position_component_type = 0;
uint32_t vs_one_const = 0;
for (size_t vi = 5; vi < vs_cache->words;)
{
uint32_t iw = vs_cache->spv[vi] >> 16;
uint32_t io = vs_cache->spv[vi] & 0xffff;
if (!iw || vi + iw > vs_cache->words)
break;
if (io == SpvOpTypeVector && iw >= 4 && vs_cache->spv[vi + 3] == 4)
{
uint32_t vector_id = vs_cache->spv[vi + 1];
uint32_t component_type = vs_cache->spv[vi + 2];
for (size_t pi = 5; pi < vs_cache->words;)
{
uint32_t pw = vs_cache->spv[pi] >> 16;
uint32_t po = vs_cache->spv[pi] & 0xffff;
if (!pw || pi + pw > vs_cache->words)
break;
if (po == SpvOpTypePointer && pw >= 4 && vs_cache->spv[pi + 2] == SpvStorageClassOutput && vs_cache->spv[pi + 3] == vector_id)
{
uint32_t pointer_id = vs_cache->spv[pi + 1];
for (size_t ai = 5; ai < vs_cache->words;)
{
uint32_t aw = vs_cache->spv[ai] >> 16;
uint32_t ao = vs_cache->spv[ai] & 0xffff;
if (!aw || ai + aw > vs_cache->words)
break;
if (ao == SpvOpAccessChain && aw >= 5 && vs_cache->spv[ai + 1] == pointer_id)
{
uint32_t result_id = vs_cache->spv[ai + 2];
uint32_t index_id = vs_cache->spv[ai + aw - 1];
bool index_zero = false;
for (size_t ci2 = 5; ci2 < vs_cache->words;)
{
uint32_t cw = vs_cache->spv[ci2] >> 16;
uint32_t co = vs_cache->spv[ci2] & 0xffff;
if (!cw || ci2 + cw > vs_cache->words)
break;
if (co == SpvOpConstant && cw >= 4 && vs_cache->spv[ci2 + 2] == index_id && vs_cache->spv[ci2 + 3] == 0)
{
index_zero = true;
break;
}
ci2 += cw;
}
if (index_zero)
{
for (size_t si = 5; si < vs_cache->words;)
{
uint32_t sw = vs_cache->spv[si] >> 16;
uint32_t so = vs_cache->spv[si] & 0xffff;
if (!sw || si + sw > vs_cache->words)
break;
if (so == SpvOpStore && sw >= 3 && vs_cache->spv[si + 1] == result_id)
{
vs_position_ptr = result_id;
vs_position_value = vs_cache->spv[si + 2];
vs_position_type = vector_id;
vs_position_component_type = component_type;
break;
}
si += sw;
}
}
}
ai += aw;
}
}
pi += pw;
}
}
vi += iw;
}
if (vs_position_value && vs_position_component_type)
{
for (size_t ci2 = 5; ci2 < vs_cache->words;)
{
uint32_t cw = vs_cache->spv[ci2] >> 16;
uint32_t co = vs_cache->spv[ci2] & 0xffff;
if (!cw || ci2 + cw > vs_cache->words)
break;
if (co == SpvOpConstant && cw >= 4 && vs_cache->spv[ci2 + 1] == vs_position_component_type && vs_cache->spv[ci2 + 3] == 0x3f800000)
{
vs_one_const = vs_cache->spv[ci2 + 2];
break;
}
ci2 += cw;
}
}
if (vs_position_value && vs_one_const)
{
for (size_t ci2 = 5; ci2 < vs_cache->words;)
{
uint32_t cw = vs_cache->spv[ci2] >> 16;
uint32_t co = vs_cache->spv[ci2] & 0xffff;
if (!cw || ci2 + cw > vs_cache->words)
break;
if (co == SpvOpCompositeConstruct && cw >= 7 && vs_cache->spv[ci2 + 1] == vs_position_type && vs_cache->spv[ci2 + 2] == vs_position_value && vs_cache->spv[ci2 + 5] == vs_one_const && vs_cache->spv[ci2 + 6] == vs_one_const)
{
vs_z_one_position = true;
break;
}
ci2 += cw;
}
}
vs_background =
(vs_quad_fs && vs_has_v3_user_output && !vm.has_v2_position_input && ci->pDepthStencilState && !ci->pDepthStencilState->depthWriteEnable)
|| (vs_quad_fs && vs_has_user_output && !vm.has_v2_position_input && vs_z_one_position && ci->pDepthStencilState && !ci->pDepthStencilState->depthWriteEnable)
|| (vs_has_user_output && vm.has_matrix_ops && !vm.has_direct_position_write && ci->pInputAssemblyState && ci->pInputAssemblyState->topology == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST && ci->pDepthStencilState && !ci->pDepthStencilState->depthWriteEnable && ((!ci->pDepthStencilState->depthTestEnable && ci->pRasterizationState && ci->pRasterizationState->cullMode == VK_CULL_MODE_NONE) || (ci->pRasterizationState && (ci->pRasterizationState->cullMode & VK_CULL_MODE_FRONT_BIT))));
vs_uv_quad = vs_quad_fs && vs_has_user_output && !vs_has_v3_user_output && !vm.has_v2_position_input && !vm.has_direct_position_write && vm.location0_count == 2 && vm.screen_value_count == 2 && !vs_z_one_position;
vs_procedural_sky = vs_background && vs_quad_fs && !vs_z_one_position;
vs_pure_quad = vs_quad_fs && !vs_has_user_output;
}
if (sd->stereo.mono_ui)
{
if (ci->pVertexInputState && ci->pVertexInputState->pVertexAttributeDescriptions)
{
for (uint32_t ui_a = 0; ui_a < ci->pVertexInputState->vertexAttributeDescriptionCount; ui_a++)
{
const VkVertexInputAttributeDescription *ui_ad = &ci->pVertexInputState->pVertexAttributeDescriptions[ui_a];
}
}
if (ci->pColorBlendState && ci->pColorBlendState->pAttachments)
{
for (uint32_t ui_bi = 0; ui_bi < ci->pColorBlendState->attachmentCount; ui_bi++)
{
const VkPipelineColorBlendAttachmentState *ui_ba = &ci->pColorBlendState->pAttachments[ui_bi];
}
}
}
bool mono_ui_menu_background = false;
if (ci->pVertexInputState && ci->pVertexInputState->vertexAttributeDescriptionCount == 2 && ci->pVertexInputState->pVertexAttributeDescriptions && ci->pVertexInputState->vertexBindingDescriptionCount == 1)
{
bool has_position = false;
bool has_color = false;
for (uint32_t ui_a = 0; ui_a < ci->pVertexInputState->vertexAttributeDescriptionCount; ui_a++)
{
const VkVertexInputAttributeDescription *ui_ad = &ci->pVertexInputState->pVertexAttributeDescriptions[ui_a];
if (ui_ad->location == 0 && ui_ad->binding == 0 && ui_ad->format == VK_FORMAT_R32G32B32_SFLOAT && ui_ad->offset == 0) has_position = true;
if (ui_ad->location == 5 && ui_ad->binding == 0 && ui_ad->format == VK_FORMAT_R8G8B8A8_UNORM && ui_ad->offset == 12) has_color = true;
}
mono_ui_menu_background = has_position && has_color;
}
if (sd->stereo.mono_ui)
{
}
bool mono_ui_depth_disabled=ci->pDepthStencilState&&!ci->pDepthStencilState->depthTestEnable&&!ci->pDepthStencilState->depthWriteEnable;
bool mono_ui_alpha_blend=false;
if(ci->pColorBlendState&&ci->pColorBlendState->pAttachments&&ci->pColorBlendState->attachmentCount>0)
{
const VkPipelineColorBlendAttachmentState *ba=&ci->pColorBlendState->pAttachments[0];
mono_ui_alpha_blend=ba->blendEnable&&ba->colorWriteMask!=0&&ba->srcColorBlendFactor==VK_BLEND_FACTOR_SRC_ALPHA&&ba->dstColorBlendFactor==VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
}
bool mono_ui_passthrough_pipeline=ci->pDepthStencilState&&!ci->pDepthStencilState->depthTestEnable&&ci->pVertexInputState&&ci->pVertexInputState->vertexBindingDescriptionCount==1&&ci->pVertexInputState->vertexAttributeDescriptionCount==3&&ci->pInputAssemblyState&&ci->pInputAssemblyState->topology==VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST&&mono_ui_alpha_blend;
bool mono_ui_passthrough_vs=vm.exec_model==SpvExecVertex&&vm.pos_is_block&&vm.has_v2_position_input&&!vm.has_matrix_ops&&!vm.has_vector_matrix_ops&&!vm.proj_found&&vm.dot_count==0&&!vm.has_direct_position_write&&!vm.has_emit_vertex&&vm.screen_has_zw_use;
bool mono_ui_always_depth_attrs=false;
if(ci->pVertexInputState&&ci->pVertexInputState->pVertexAttributeDescriptions&&ci->pVertexInputState->vertexBindingDescriptionCount==1&&ci->pVertexInputState->vertexAttributeDescriptionCount==3)
{
bool has_position=false;
bool has_color=false;
bool has_texcoord=false;
for(uint32_t ui_a=0;ui_a<ci->pVertexInputState->vertexAttributeDescriptionCount;ui_a++)
{
const VkVertexInputAttributeDescription *ui_ad=&ci->pVertexInputState->pVertexAttributeDescriptions[ui_a];
if(ui_ad->location==0&&ui_ad->binding==0&&ui_ad->format==VK_FORMAT_R32G32B32_SFLOAT&&ui_ad->offset==0)has_position=true;
if(ui_ad->location==1&&ui_ad->binding==0&&ui_ad->format==VK_FORMAT_R8G8B8A8_UNORM&&ui_ad->offset==12)has_color=true;
if(ui_ad->location==2&&ui_ad->binding==0&&ui_ad->offset==16)has_texcoord=true;
}
mono_ui_always_depth_attrs=has_position&&has_color&&has_texcoord;
}
bool mono_ui_always_depth_pipeline=ci->pDepthStencilState&&ci->pDepthStencilState->depthTestEnable&&!ci->pDepthStencilState->depthWriteEnable&&ci->pDepthStencilState->depthCompareOp==VK_COMPARE_OP_ALWAYS&&ci->pRasterizationState&&ci->pRasterizationState->cullMode==VK_CULL_MODE_NONE&&ci->pInputAssemblyState&&ci->pInputAssemblyState->topology==VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST&&mono_ui_always_depth_attrs&&ci->pColorBlendState&&ci->pColorBlendState->pAttachments&&ci->pColorBlendState->attachmentCount>0&&!ci->pColorBlendState->pAttachments[0].blendEnable&&ci->pColorBlendState->pAttachments[0].colorWriteMask!=0;
bool mono_ui_always_depth_vs=vm.exec_model==SpvExecVertex&&vm.proj_found&&vm.dot_count>=8&&vm.has_direct_position_write&&vm.location0_count==4&&vm.screen_value_count==4&&vm.screen_has_zw_use&&!vm.has_emit_vertex;
bool mono_ui_gs_signature=gs_input_count==7&&gs_output_mode==SpvExecutionModeOutputTriangleStrip&&gs_output_vertices==4;
bool mono_ui_gs_pipeline_state=ci->pDepthStencilState&&!ci->pDepthStencilState->depthTestEnable&&ci->pVertexInputState&&ci->pVertexInputState->vertexBindingDescriptionCount==1&&ci->pVertexInputState->vertexAttributeDescriptionCount==7;
bool mono_ui_gs_candidate=sd->stereo.mono_ui&&has_gs&&mono_ui_gs_signature&&mono_ui_gs_pipeline_state;
bool mono_ui_always_depth_ui_state=ci->pDepthStencilState&&ci->pDepthStencilState->depthTestEnable&&!ci->pDepthStencilState->depthWriteEnable&&ci->pDepthStencilState->depthCompareOp==VK_COMPARE_OP_ALWAYS&&ci->pInputAssemblyState&&ci->pInputAssemblyState->topology==VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
bool mono_ui_vs_candidate=sd->stereo.mono_ui&&((mono_ui_depth_disabled&&(vs_screen_space||(mono_ui_menu_background&&vs_quad_fs&&!vm.has_matrix_ops&&!vm.has_direct_position_write)))||(mono_ui_passthrough_vs&&mono_ui_passthrough_pipeline)||(mono_ui_always_depth_vs&&mono_ui_always_depth_ui_state));
if(mono_ui_gs_candidate||mono_ui_vs_candidate)
{
free_spv_provenance(&vm);
continue;
}
free_spv_provenance(&vm);
}
}
if (((vs_fullscreen && !vs_screen_space) || (is_quad && (vs_quad_fs || vs_screen_quad)) || (!has_vs && (gpl_flags & VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_SHADER_BIT_EXT) != 0)) &&
!has_ms &&
!has_gs &&
!has_tes &&
!has_tcs &&
in_mv_rp &&
ci->stageCount > 0 &&
fs_stage != ~0u)
{
uint32_t fs_s = ~0u;
for (uint32_t s2 = 0; s2 < ci->stageCount; s2++)
{
if (ci->pStages[s2].stage == VK_SHADER_STAGE_FRAGMENT_BIT)
{
fs_s = s2;
break;
}
}
if (fs_s == ~0u)
{
continue;
}
StereoShaderCache *fs_cache =
cache_find(sd, ci->pStages[fs_s].module);
StereoShaderCache inline_fs_cache = {0};
const VkShaderModuleCreateInfo *fs_inline =
stereo_stage_inline_spv(&ci->pStages[fs_s]);
if (!fs_cache && fs_inline && fs_inline->pCode &&
fs_inline->codeSize >= 20 &&
(fs_inline->codeSize & 3) == 0)
{
inline_fs_cache.spv = (uint32_t *)fs_inline->pCode;
inline_fs_cache.words = fs_inline->codeSize / 4;
fs_cache = &inline_fs_cache;
}
if (!fs_cache)
{
for (uint32_t k = 0; k < sd->shader_cache_count; ++k)
{
}
continue;
}
uint64_t spv_hash =
hash_spv(fs_cache->spv, fs_cache->words);
uint32_t pipeline_has_mv =
(rpi != NULL) ? (uint32_t)rpi->has_multiview : 0;
if (dump)
{
char dp[512];
_snprintf(
dp,
sizeof(dp) - 1,
"%s\\%016llx-fs.spv",
dump,
(unsigned long long)spv_hash);
FILE *f = fopen(dp, "rb");
if (!f)
{
f = fopen(dp, "wb");
if (f)
{
fwrite(
fs_cache->spv,
4,
fs_cache->words,
f);
fclose(f);
}
}
else
{
fclose(f);
}
}
uint32_t *patched = NULL; size_t pc2 = 0;
{
SpvMod fm = {0};
fm.words = fs_cache->spv;
fm.count = fs_cache->words;
fm.bound = fm.words[3];
fm.value_capacity = fm.bound + 64;
fm.value_from_matrix =
calloc(fm.value_capacity, sizeof(uint8_t));
fm.is_matrix_type =
calloc(fm.value_capacity, sizeof(uint8_t));
fm.is_matrix_ptr =
calloc(fm.value_capacity, sizeof(uint8_t));
fm.is_proj_value =
calloc(fm.value_capacity, sizeof(uint8_t));
fm.is_view_value =
calloc(fm.value_capacity, sizeof(uint8_t));
if (fm.value_from_matrix &&
fm.is_matrix_type &&
fm.is_matrix_ptr &&
fm.is_proj_value &&
fm.is_view_value)
{
spv_scan(&fm);
if (fm.proj_found)
{
dbg_out[p].has_proj_ubo = true;
dbg_out[p].proj_set = fm.proj_set;
dbg_out[p].proj_binding = fm.proj_binding;
dbg_out[p].proj_member_mask =
fm.proj_member_mask;
dbg_out[p].proj_var = fm.proj_var;
}
}
free_spv_provenance(&fm);
}
if (stereo_skip_shader_patch(spv_hash))
{
continue;
}
bool fs_patched = spirv_patch_stereo_fs(
fs_cache->spv,
fs_cache->words,
&patched,
&pc2);
if (!fs_patched)
{
}
else
{
if (dump) {
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
if (stereo_stage_inline_spv(&st[fs_s]))
st[fs_s].pNext = NULL;
infos[p].pStages = st;
tmp_mod[p] = tmp;
tst[p] = st;
infos[p].renderPass = pipeline_rp;
continue;
}
}
if (in_mv_rp &&
has_ms &&
ms_stage != ~0u) {
VkShaderModule ms_module =
ci->pStages[ms_stage].module;
StereoShaderCache *e =
cache_find(sd, ms_module);
if (!e) {
for (uint32_t k = 0; k < sd->shader_cache_count; ++k) {
}
continue;
}
if (e) {
size_t scan_i = 5;
while (scan_i < e->words) {
uint32_t iw = e->spv[scan_i] >> 16;
uint32_t io = e->spv[scan_i] & 0xffff;
if (!iw || scan_i + iw > e->words)
break;
if (io == SpvOpEntryPoint && iw >= 3) {
}
scan_i += iw;
}
}
uint64_t spv_hash = hash_spv(e->spv, e->words);
if (dump)
{
char dp[512];
_snprintf(
dp,
sizeof(dp)-1,
"%s\\%016llx-ms.spv",
dump,
(unsigned long long)spv_hash);
FILE *f = fopen(dp, "rb");
if (!f)
{
f = fopen(dp, "wb");
if (f)
{
fwrite(e->spv, 4, e->words, f);
fclose(f);
}
}
else
{
fclose(f);
}
}
uint32_t *patched = NULL;
size_t pc2 = 0;
StereoDebugCtx *dbgM = &dbg_out[p];
*dbgM = (StereoDebugCtx){
p,
ci->renderPass,
in_mv_rp,
(uint32_t)VK_SHADER_STAGE_MESH_BIT_EXT,
0,
0,
false,
false
};
if (stereo_skip_shader_patch(spv_hash))
{
continue;
}
if (!spirv_patch_stereo_mesh(
&sd->stereo,
e->spv,
e->words,
&patched,
&pc2,
lo,
ro,
conv,
true,
dbgM))
{
if (dump && patched && pc2)
{
char dp[512];
_snprintf(
dp,
sizeof(dp)-1,
"%s\\%016llx-ms_failed.spv",
dump,
(unsigned long long)spv_hash);
FILE *f = fopen(dp, "wb");
if (f)
{
fwrite(patched, 4, pc2, f);
fclose(f);
}
}
continue;
}
if (dump)
{
char dp[512];
_snprintf(
dp,
sizeof(dp)-1,
"%s\\%016llx+ms.spv",
dump,
(unsigned long long)spv_hash);
FILE *f = fopen(dp, "wb");
if (f)
{
fwrite(patched, 4, pc2, f);
fclose(f);
}
}
VkShaderModuleCreateInfo smci = {
VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
NULL,
0,
pc2 * 4,
patched
};
VkShaderModule tmp = VK_NULL_HANDLE;
VkResult mr = sd->real.CreateShaderModule(
sd->real_device,
&smci,
NULL,
&tmp);
spirv_patched_free(patched);
if (mr != VK_SUCCESS)
{
STEREO_ERR(
"Pipe %u Mesh: module err %d",
p,
mr);
continue;
}
uint32_t sc = ci->stageCount;
VkPipelineShaderStageCreateInfo *st =
malloc(sc * sizeof(*st));
if (!st)
{
sd->real.DestroyShaderModule(
sd->real_device,
tmp,
NULL);
continue;
}
memcpy(st, ci->pStages, sc * sizeof(*st));
st[ms_stage].module = tmp;
infos[p].pStages = st;
tmp_mod[p] = tmp;
tst[p] = st;
infos[p].renderPass = pipeline_rp;
continue;
}
if (has_gs && gs_stage != ~0u) {
StereoShaderCache *e =
cache_find(sd, ci->pStages[gs_stage].module);
if (!e) {
continue;
}
uint64_t spv_hash = hash_spv(e->spv, e->words);
if (dump)
{
char dp[512];
_snprintf(
dp,
sizeof(dp)-1,
"%s\\%016llx-gs.spv",
dump,
(unsigned long long)spv_hash);
FILE *f = fopen(dp, "rb");
if (!f)
{
f = fopen(dp, "wb");
if (f)
{
fwrite(e->spv, 4, e->words, f);
fclose(f);
}
}
else
{
fclose(f);
}
}
uint32_t *patched = NULL;
size_t pc2 = 0;
StereoDebugCtx *dbgG = &dbg_out[p];
*dbgG = (StereoDebugCtx){
p,
ci->renderPass,
in_mv_rp,
(uint32_t)VK_SHADER_STAGE_GEOMETRY_BIT,
0,
0,
false,
false
};
if (!spirv_patch_stereo_vertex(
&sd->stereo,
e->spv, e->words,
&patched, &pc2,
lo, ro, conv,
true,
false,
false,
dbgG))
{
continue;
}
if (dump) {
char dp[512];
_snprintf(
dp,
sizeof(dp)-1,
"%s\\%016llx+gs.spv",
dump,
(unsigned long long)spv_hash);
FILE *f=fopen(dp,"wb");
if (f) {
fwrite(patched,4,pc2,f);
fclose(f);
}
}
VkShaderModuleCreateInfo smci={
VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
NULL,0,pc2*4,patched};
VkShaderModule tmp=VK_NULL_HANDLE;
VkResult mr=sd->real.CreateShaderModule(
sd->real_device,&smci,NULL,&tmp);
spirv_patched_free(patched);
if (mr!=VK_SUCCESS) {
STEREO_ERR(
"Pipe %u PathGS: module err %d",
p,mr);
continue;
}
uint32_t sc=ci->stageCount;
VkPipelineShaderStageCreateInfo *st=
malloc(sc*sizeof(*st));
if (!st) {
sd->real.DestroyShaderModule(
sd->real_device,tmp,NULL);
continue;
}
memcpy(st,ci->pStages,sc*sizeof(*st));
st[gs_stage].module = tmp;
infos[p].pStages = st;
tmp_mod[p] = tmp;
tst[p] = st;
infos[p].renderPass = pipeline_rp;
continue;
}
if (has_tes && tes_stage!=~0u) {
StereoShaderCache *e=cache_find(sd, ci->pStages[tes_stage].module);
if (!e) {}
if (dump)
{
uint64_t spv_hash=hash_spv(e->spv, e->words);
char dp[512];
_snprintf(
dp,
sizeof(dp)-1,
"%s\\%016llx-ts.spv",
dump,
(unsigned long long)spv_hash);
FILE *f = fopen(dp, "rb");
if (!f)
{
f = fopen(dp, "wb");
if (f)
{
fwrite(
e->spv,
4,
e->words,
f);
fclose(f);
}
}
else
{
fclose(f);
}
}
uint32_t *patched=NULL; size_t pc2=0;
StereoDebugCtx dbgA = {
p,
ci->renderPass,
in_mv_rp,
(uint32_t)VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT,
0,
0
};
if (!spirv_patch_stereo_vertex(
&sd->stereo,
e->spv, e->words,
&patched, &pc2,
lo, ro, conv,
true,
false,
false,
&dbgA))
{
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
infos[p].renderPass = pipeline_rp;
continue;
}
if (in_mv_rp &&
ci->stageCount > 0 &&
has_vs &&
!has_tcs &&
vs_stage != ~0u &&
!vs_pure_quad &&
!vs_screen_quad) 
{
StereoShaderCache inline_e;
StereoShaderCache *e=cache_find(sd, ci->pStages[vs_stage].module);
const VkShaderModuleCreateInfo *inline_smci=stereo_stage_inline_spv(&ci->pStages[vs_stage]);
if (!e && inline_smci && inline_smci->pCode && inline_smci->codeSize >= 4)
{
inline_e.spv=(uint32_t *)inline_smci->pCode;
inline_e.words=inline_smci->codeSize/4;
e=&inline_e;
}
if (!e) {}
if (dump)
{
uint64_t spv_hash=hash_spv(e->spv, e->words);
char dp[512];
_snprintf(
dp,
sizeof(dp)-1,
"%s\\%016llx-vs.spv",
dump,
(unsigned long long)spv_hash);
FILE *f = fopen(dp, "rb");
if (!f)
{
f = fopen(dp, "wb");
if (f)
{
fwrite(
e->spv,
4,
e->words,
f);
fclose(f);
}
}
else
{
fclose(f);
}
}
uint32_t *patched=NULL; size_t pc2=0;
StereoDebugCtx *dbgB = &dbg_out[p];
*dbgB = (StereoDebugCtx){
p,
ci->renderPass,
in_mv_rp,
(uint32_t)VK_SHADER_STAGE_VERTEX_BIT,
0,
9,
false,
false
};
bool procedural_sky = vs_procedural_sky && sd->stereo.sky_extend;
bool flatten_sky = vs_background && !vs_procedural_sky && sd->stereo.sky_flatten;
bool force_sky_depth = flatten_sky || procedural_sky;
if (vs_uv_quad)
{
continue;
}
if (!spirv_patch_stereo_vertex(
&sd->stereo,
e->spv, e->words,
&patched, &pc2,
lo, ro, conv,
true,
force_sky_depth,
procedural_sky,
dbgB)) {
continue;
}
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
if (stereo_stage_inline_spv(&st[vs_stage]))
st[vs_stage].pNext = NULL;
infos[p].pStages = st;
tmp_mod[p] = tmp;
tst[p] = st;
infos[p].renderPass = pipeline_rp;
continue;
}
}
for (uint32_t p = 0; p < N; p++) {
StereoRenderPassInfo *rpi = NULL;
VkRenderPass pipeline_rp = pCI[p].renderPass;
if (pCI[p].renderPass != VK_NULL_HANDLE)
rpi = stereo_rp_lookup(sd, pCI[p].renderPass);
if (rpi &&
rpi->has_multiview &&
rpi->mv_handle != VK_NULL_HANDLE)
{
pipeline_rp = rpi->mv_handle;
}
bool pipeline_patched =
tst[p] != NULL;
if (pipeline_rp != pCI[p].renderPass)
infos[p].renderPass = pipeline_rp;
else
infos[p].renderPass = pCI[p].renderPass;
}
for (uint32_t p = 0; p < N; p++) {
VkPipelineCreateFlags2CreateInfoKHR *flags2_ci=NULL;
VkGraphicsPipelineLibraryCreateInfoEXT *gpl_ci=NULL;
VkPipelineLibraryCreateInfoKHR *lib_ci=NULL;
for (const VkBaseInStructure *x=(const VkBaseInStructure *)pCI[p].pNext;x;x=x->pNext) {
if (x->sType==VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_LIBRARY_CREATE_INFO_EXT) gpl_ci=(VkGraphicsPipelineLibraryCreateInfoEXT *)x;
if (x->sType==VK_STRUCTURE_TYPE_PIPELINE_LIBRARY_CREATE_INFO_KHR) lib_ci=(VkPipelineLibraryCreateInfoKHR *)x;
if (x->sType==VK_STRUCTURE_TYPE_PIPELINE_CREATE_FLAGS_2_CREATE_INFO_KHR) flags2_ci=(VkPipelineCreateFlags2CreateInfoKHR *)x;
}
}
for (uint32_t p = 0; p < N; ++p)
{
for (uint32_t s = 0; s < infos[p].stageCount; s++)
{
const VkPipelineShaderStageCreateInfo *pst=&infos[p].pStages[s];
const VkBaseInStructure *sx=(const VkBaseInStructure *)pst->pNext;
uint32_t has_identifier=0;
while (sx)
{
if (sx->sType==VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_MODULE_IDENTIFIER_CREATE_INFO_EXT)
{
const VkPipelineShaderStageModuleIdentifierCreateInfoEXT *mi=(const VkPipelineShaderStageModuleIdentifierCreateInfoEXT *)sx;
has_identifier=1;
}
sx=sx->pNext;
}
}
}
for (uint32_t p = 0; p < N; ++p)
{
for (uint32_t s = 0; s < infos[p].stageCount; ++s)
{
const VkPipelineShaderStageCreateInfo *st =
&infos[p].pStages[s];
}
}
for (uint32_t dbg_p = 0; dbg_p < N; ++dbg_p) {
for (uint32_t dbg_s = 0; dbg_s < infos[dbg_p].stageCount; ++dbg_s) {
const VkPipelineShaderStageCreateInfo *dbg_st =
&infos[dbg_p].pStages[dbg_s];
}
}
uint64_t *original_vs_hash = calloc(N, sizeof(*original_vs_hash));
if (!original_vs_hash)
{
for (uint32_t p=0;p<N;p++)
{
if (tmp_mod[p])
sd->real.DestroyShaderModule(sd->real_device,tmp_mod[p],NULL);
free(tst[p]);
}
free(tmp_mod); free(tst); free(infos);
return VK_ERROR_OUT_OF_HOST_MEMORY;
}
for (uint32_t p=0;p<N;p++)
{
for (uint32_t s=0;s<pCI[p].stageCount;s++)
{
const VkPipelineShaderStageCreateInfo *st=&pCI[p].pStages[s];
if (st->stage==VK_SHADER_STAGE_VERTEX_BIT)
{
StereoShaderCache *vs_cache=cache_find(sd,st->module);
if (vs_cache)
original_vs_hash[p]=hash_spv(vs_cache->spv,vs_cache->words);
break;
}
}
}
VkResult res=sd->real.CreateGraphicsPipelines(sd->real_device,pc,N,infos,pAlloc,pP);
for (uint32_t p = 0; p < N; p++) {
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
info->vs_hash = 0;
info->depth_test_enable = pCI[p].pDepthStencilState ? pCI[p].pDepthStencilState->depthTestEnable : VK_FALSE;
info->depth_write_enable = pCI[p].pDepthStencilState ? pCI[p].pDepthStencilState->depthWriteEnable : VK_FALSE;
info->depth_compare_op = pCI[p].pDepthStencilState ? pCI[p].pDepthStencilState->depthCompareOp : VK_COMPARE_OP_ALWAYS;
info->blend_enable = pCI[p].pColorBlendState && pCI[p].pColorBlendState->attachmentCount && pCI[p].pColorBlendState->pAttachments[0].blendEnable;
info->view_mask = 0; 
info->has_proj_ubo = dbg_out[p].has_proj_ubo;
info->proj_set = dbg_out[p].proj_set;
info->proj_binding = dbg_out[p].proj_binding;
info->proj_member_mask = dbg_out[p].proj_member_mask;
info->proj_var = dbg_out[p].proj_var;
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
info->vs_hash = original_vs_hash[p];
}
if (st->stage == VK_SHADER_STAGE_GEOMETRY_BIT)
{
info->gs_module = st->module;
}
if (st->stage == VK_SHADER_STAGE_MESH_BIT_EXT)
{
info->ms_module = st->module;
info->patched_ms =
(tmp_mod[p] != VK_NULL_HANDLE &&
st->module == tmp_mod[p]);
}
if (st->stage == VK_SHADER_STAGE_FRAGMENT_BIT)
{
info->fs_module = st->module;
info->patched_fs =
(tmp_mod[p] != VK_NULL_HANDLE &&
st->module == tmp_mod[p]);
StereoShaderCache *fs_cache = cache_find(sd, st->module);
if (fs_cache)
{
uint64_t h = hash_spv(fs_cache->spv, fs_cache->words);
}
}
}
}
}
}
for (uint32_t p=0;p<N;p++) {
if (tmp_mod[p]) {
if (sd->tmp_module_count<MAX_TMP_MODULES)
sd->tmp_modules[sd->tmp_module_count++]=tmp_mod[p];
else
sd->real.DestroyShaderModule(sd->real_device,tmp_mod[p],NULL);
}
free(tst[p]);
}
free(original_vs_hash);
free(tmp_mod); free(tst); free(infos);
return res;
}
VKAPI_ATTR VkResult VKAPI_CALL
stereo_CreateRayTracingPipelinesKHR(
VkDevice device,
VkDeferredOperationKHR deferredOperation,
VkPipelineCache pipelineCache,
uint32_t createInfoCount,
const VkRayTracingPipelineCreateInfoKHR *pCreateInfos,
const VkAllocationCallbacks *pAllocator,
VkPipeline *pPipelines)
{
StereoDevice *sd = stereo_device_from_handle(device);
if (!sd)
return VK_ERROR_DEVICE_LOST;
if (!sd->real.CreateRayTracingPipelinesKHR)
{
STEREO_ERR("RT_PIPE_CREATE missing real dispatch");
return VK_ERROR_INITIALIZATION_FAILED;
}
VkRayTracingPipelineCreateInfoKHR *patched_ci =
calloc(createInfoCount, sizeof(*patched_ci));
VkPipelineShaderStageCreateInfo **patched_stages =
calloc(createInfoCount, sizeof(*patched_stages));
VkShaderModule *tmp_raygen_modules =
calloc(createInfoCount, sizeof(*tmp_raygen_modules));
if (!patched_ci || !patched_stages || !tmp_raygen_modules)
{
free(patched_ci);
free(patched_stages);
free(tmp_raygen_modules);
return VK_ERROR_OUT_OF_HOST_MEMORY;
}
for (uint32_t p = 0; p < createInfoCount; p++)
{
const VkRayTracingPipelineCreateInfoKHR *ci = &pCreateInfos[p];
patched_ci[p] = *ci;
for (uint32_t s = 0; s < ci->stageCount; s++)
{
const VkPipelineShaderStageCreateInfo *st = &ci->pStages[s];
if (st->stage != VK_SHADER_STAGE_RAYGEN_BIT_KHR)
continue;
StereoShaderCache *cache = cache_find(sd, st->module);
if (!cache)
{
continue;
}
const char *dump = stereo_getenv("VKS3D_DUMP_SPIRV");
if (dump)
{
uint64_t spv_hash = hash_spv(cache->spv, cache->words);
char dp[512];
_snprintf(
dp,
sizeof(dp) - 1,
"%s\\%016llx-rs.spv",
dump,
(unsigned long long)spv_hash);
FILE *f = fopen(dp, "rb");
if (!f)
{
f = fopen(dp, "wb");
if (f)
{
fwrite(cache->spv, 4, cache->words, f);
fclose(f);
}
}
else
{
fclose(f);
}
}
uint32_t *patched_spv = NULL;
size_t patched_words = 0;
log_rt_raygen_spv(cache->spv, cache->words);
if (!spirv_patch_stereo_raygen(
cache->spv,
cache->words,
&patched_spv,
&patched_words,
sd->stereo.left_eye_offset,
sd->stereo.right_eye_offset,
sd->stereo.convergence,
sd->stereo.projection))
{
continue;
}
log_rt_raygen_spv(patched_spv, patched_words);
if (dump)
{
uint64_t spv_hash = hash_spv(cache->spv, cache->words);
char dp[512];
_snprintf(
dp,
sizeof(dp) - 1,
"%s\\%016llx+rs.spv",
dump,
(unsigned long long)spv_hash);
FILE *f = fopen(dp, "rb");
if (!f)
{
f = fopen(dp, "wb");
if (f)
{
fwrite(patched_spv, 4, patched_words, f);
fclose(f);
}
}
else
{
fclose(f);
}
}
VkShaderModuleCreateInfo smci = {
.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
.codeSize = patched_words * sizeof(uint32_t),
.pCode = patched_spv
};
VkShaderModule tmp_module = VK_NULL_HANDLE;
VkResult patch_res = sd->real.CreateShaderModule(
sd->real_device,
&smci,
NULL,
&tmp_module);
spirv_patched_free(patched_spv);
if (patch_res != VK_SUCCESS)
{
continue;
}
patched_stages[p] =
calloc(ci->stageCount, sizeof(*patched_stages[p]));
if (!patched_stages[p])
{
sd->real.DestroyShaderModule(
sd->real_device,
tmp_module,
NULL);
free(patched_ci);
free(patched_stages);
free(tmp_raygen_modules);
return VK_ERROR_OUT_OF_HOST_MEMORY;
}
memcpy(
patched_stages[p],
ci->pStages,
ci->stageCount * sizeof(*patched_stages[p]));
patched_stages[p][s].module = tmp_module;
patched_stages[p][s].pNext=NULL;
patched_ci[p].pStages = patched_stages[p];
tmp_raygen_modules[p] = tmp_module;
break;
}
for (uint32_t s = 0; s < ci->stageCount; s++)
{
const VkPipelineShaderStageCreateInfo *st =
patched_stages[p] ?
&patched_stages[p][s] :
&ci->pStages[s];
}
for (uint32_t g = 0; g < ci->groupCount; g++)
{
const VkRayTracingShaderGroupCreateInfoKHR *grp =
&ci->pGroups[g];
}
}
VkResult res = sd->real.CreateRayTracingPipelinesKHR(
sd->real_device,
deferredOperation,
pipelineCache,
createInfoCount,
patched_ci,
pAllocator,
pPipelines);
for (uint32_t p = 0; p < createInfoCount; p++)
{
if (tmp_raygen_modules[p] != VK_NULL_HANDLE)
{
if (sd->tmp_module_count < MAX_TMP_MODULES)
{
sd->tmp_modules[sd->tmp_module_count++] =
tmp_raygen_modules[p];
}
else
{
sd->real.DestroyShaderModule(
sd->real_device,
tmp_raygen_modules[p],
NULL);
}
}
free(patched_stages[p]);
}
free(tmp_raygen_modules);
free(patched_stages);
free(patched_ci);
return res;
}
VKAPI_ATTR void VKAPI_CALL
stereo_DestroyShaderModule(VkDevice device, VkShaderModule sm,
const VkAllocationCallbacks *pAlloc)
{
StereoDevice *sd=stereo_device_from_handle(device);
if (!sd) return;
cache_remove(sd,sm);
sd->real.DestroyShaderModule(sd->real_device,sm,pAlloc);
}
VKAPI_ATTR VkResult VKAPI_CALL
stereo_CreateShadersEXT(
VkDevice device,
uint32_t createInfoCount,
const VkShaderCreateInfoEXT *pCreateInfos,
const VkAllocationCallbacks *pAllocator,
VkShaderEXT *pShaders)
{
StereoDevice *sd=stereo_device_from_handle(device);
if (!sd || !sd->real.CreateShadersEXT)
return VK_ERROR_EXTENSION_NOT_PRESENT;
if (!sd->stereo.enabled)
return sd->real.CreateShadersEXT(
sd->real_device,
createInfoCount,
pCreateInfos,
pAllocator,
pShaders);
VkShaderCreateInfoEXT *patched_infos = NULL;
bool *patched_owned = NULL;
if (createInfoCount) {
patched_infos = calloc(createInfoCount, sizeof(*patched_infos));
patched_owned = calloc(createInfoCount, sizeof(*patched_owned));
if (!patched_infos || !patched_owned) {
free(patched_infos);
free(patched_owned);
return VK_ERROR_OUT_OF_HOST_MEMORY;
}
memcpy(patched_infos, pCreateInfos,
(size_t)createInfoCount * sizeof(*patched_infos));
}
for (uint32_t i=0; i<createInfoCount; i++) {
const VkShaderCreateInfoEXT *ci=&pCreateInfos[i];
}
for (uint32_t i=0; i<createInfoCount; i++) {
const VkShaderCreateInfoEXT *ci=&pCreateInfos[i];
if (ci->codeType == VK_SHADER_CODE_TYPE_BINARY_EXT) {
return VK_ERROR_INCOMPATIBLE_SHADER_BINARY_EXT;
}
if (!ci->pCode || ci->codeSize < 20) {
continue;
}
if (ci->codeType != VK_SHADER_CODE_TYPE_SPIRV_EXT) {
const uint32_t *probe = (const uint32_t *)ci->pCode;
continue;
}
const uint32_t *in = (const uint32_t *)ci->pCode;
size_t in_words = ci->codeSize / sizeof(uint32_t);
uint32_t *patched = NULL;
size_t out_words = 0;
bool ok = false;
uint64_t spv_hash = hash_spv(in, in_words);
if (stereo_skip_shader_patch(spv_hash))
{
ok = false;
patched = NULL;
out_words = 0;
}
else if (ci->stage == VK_SHADER_STAGE_VERTEX_BIT) {
ok = spirv_patch_stereo_vertex(
&sd->stereo,
in,
in_words,
&patched,
&out_words,
sd->stereo.left_eye_offset,
sd->stereo.right_eye_offset,
sd->stereo.convergence,
true,
false,
false,
NULL);
} else if (ci->stage == VK_SHADER_STAGE_FRAGMENT_BIT) {
ok = spirv_patch_stereo_fs(
in,
in_words,
&patched,
&out_words);
}
const char *dump = getenv("VKS3D_DUMP_SPIRV");
if (dump &&
(ci->stage == VK_SHADER_STAGE_VERTEX_BIT ||
ci->stage == VK_SHADER_STAGE_FRAGMENT_BIT)) {
uint64_t spv_hash = hash_spv(in, in_words);
char path[512];
const char *stage_suffix =
ci->stage == VK_SHADER_STAGE_VERTEX_BIT ? "-vso.spv" : "-fso.spv";
_snprintf_s(
path,
sizeof(path),
_TRUNCATE,
"%s\\%016llx%s",
dump,
(unsigned long long)spv_hash,
stage_suffix);
FILE *fp = fopen(path, "rb");
if (!fp) {
fp = fopen(path, "wb");
if (fp) {
fwrite(in, sizeof(uint32_t), in_words, fp);
fclose(fp);
}
} else {
fclose(fp);
}
}
if (ok && patched && out_words) {
patched_infos[i].pCode = patched;
patched_infos[i].codeSize =
out_words * sizeof(uint32_t);
patched_owned[i] = true;
if (dump) {
uint64_t spv_hash = hash_spv(patched, out_words);
char path[512];
const char *stage_suffix =
ci->stage == VK_SHADER_STAGE_VERTEX_BIT ? "+vso.spv" :
ci->stage == VK_SHADER_STAGE_FRAGMENT_BIT ? "+fso.spv" : "+so.spv";
_snprintf_s(
path,
sizeof(path),
_TRUNCATE,
"%s\\%016llx%s",
dump,
(unsigned long long)spv_hash,
stage_suffix);
FILE *fp = fopen(path, "rb");
if (!fp) {
fp = fopen(path, "wb");
if (fp) {
fwrite(patched, sizeof(uint32_t), out_words, fp);
fclose(fp);
}
} else {
fclose(fp);
}
}
} else if (ci->stage == VK_SHADER_STAGE_VERTEX_BIT ||
ci->stage == VK_SHADER_STAGE_FRAGMENT_BIT) {
}
}
for (uint32_t i=0; i<createInfoCount; i++) {
}
VkResult res=sd->real.CreateShadersEXT(
sd->real_device,
createInfoCount,
patched_infos ? patched_infos : pCreateInfos,
pAllocator,
pShaders);
for (uint32_t i=0; i<createInfoCount; i++) {
if (patched_owned[i])
free((void *)patched_infos[i].pCode);
}
free(patched_owned);
free(patched_infos);
if (res == VK_SUCCESS && pShaders) {
for (uint32_t i=0; i<createInfoCount; i++) {
}
}
return res;
}
VKAPI_ATTR void VKAPI_CALL
stereo_DestroyShaderEXT(
VkDevice device,
VkShaderEXT shader,
const VkAllocationCallbacks *pAllocator)
{
StereoDevice *sd=stereo_device_from_handle(device);
if (!sd || !sd->real.DestroyShaderEXT)
return;
sd->real.DestroyShaderEXT(
sd->real_device,
shader,
pAllocator);
}