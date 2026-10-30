#version 450
// Deadcast: Naomi 2's polygons' vertex shader, compiled to SPIR-V by the build once for each pp_Gouraud
// (0, 1) and pp_Texture (0, 1)
#extension GL_GOOGLE_include_directive : require
#if pp_Gouraud == 0
#define INTERPOLATION flat
#else
#define INTERPOLATION
#endif

layout (std140, set = 0, binding = 0) uniform VertexShaderUniforms
{
	mat4 ndcMat;
} uniformBuffer;

#include "n2_light.glsl"

layout (location = 0) in vec4         in_pos;
layout (location = 1) in vec4         in_base;
layout (location = 2) in vec4         in_offs;
layout (location = 3) in mediump vec2 in_uv;
layout (location = 4) in vec3         in_normal;

layout (location = 0) INTERPOLATION out highp vec4 vtx_base;
layout (location = 1) INTERPOLATION out highp vec4 vtx_offs;
layout (location = 2) out highp vec3 vtx_uv;

void wDivide(inout vec4 vpos)
{
	vpos = vec4(vpos.xy / vpos.w, 1.0 / vpos.w, 1.0);
	vpos = uniformBuffer.ndcMat * vpos;
#if pp_Gouraud == 1
	vtx_base *= vpos.z;
	vtx_offs *= vpos.z;
#endif
	vtx_uv = vec3(vtx_uv.xy * vpos.z, vpos.z);
	vpos.w = 1.0;
	vpos.z = 0.0;
}

void main()
{
	vec4 vpos = n2Uniform.mvMat * in_pos;
	vtx_base = in_base;
	vtx_offs = in_offs;

	vec3 vnorm = normalize(mat3(n2Uniform.normalMat) * in_normal);

	// TODO bump mapping
	if (n2Uniform.bumpMapping == 0)
	{
		computeColors(vtx_base, vtx_offs, 0, vpos.xyz, vnorm);
#if pp_Texture == 0
		vtx_base += vtx_offs;
#endif
	}

	vtx_uv.xy = in_uv;
	if (n2Uniform.envMapping[0] == 1)
		computeEnvMap(vtx_uv.xy, vpos.xyz, vnorm);

	vpos = n2Uniform.projMat * vpos;
	wDivide(vpos);

	gl_Position = vpos;
}
