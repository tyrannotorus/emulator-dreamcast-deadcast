#version 450
// Deadcast: the polygons' vertex shader, compiled to SPIR-V by the build once for each pp_Gouraud (0, 1)
// and DIV_POS_Z (0, 1)
#if pp_Gouraud == 0
#define INTERPOLATION flat
#else
#define INTERPOLATION
#endif

layout (std140, set = 0, binding = 0) uniform VertexShaderUniforms
{
	mat4 ndcMat;
} uniformBuffer;

layout (location = 0) in vec4         in_pos;
layout (location = 1) in vec4        in_base;
layout (location = 2) in vec4        in_offs;
layout (location = 3) in mediump vec2 in_uv;

layout (location = 0) INTERPOLATION out highp vec4 vtx_base;
layout (location = 1) INTERPOLATION out highp vec4 vtx_offs;
layout (location = 2) out highp vec3 vtx_uv;

void main()
{
	vec4 vpos = uniformBuffer.ndcMat * in_pos;
#if DIV_POS_Z == 1
	vpos /= vpos.z;
	vpos.z = vpos.w;
#endif
	vtx_base = in_base;
	vtx_offs = in_offs;
	vtx_uv = vec3(in_uv, vpos.z);
#if pp_Gouraud == 1 && DIV_POS_Z != 1
	vtx_base *= vpos.z;
	vtx_offs *= vpos.z;
#endif

#if DIV_POS_Z != 1
	vtx_uv.xy *= vpos.z;
	vpos.w = 1.0;
	vpos.z = 0.0;
#endif
	gl_Position = vpos;
}
