#version 450
// Deadcast: the modifier volumes' vertex shader, compiled to SPIR-V by the build once for each DIV_POS_Z (0, 1)
layout (std140, set = 0, binding = 0) uniform VertexShaderUniforms
{
	mat4 ndcMat;
} uniformBuffer;

layout (location = 0) in vec4 in_pos;
layout (location = 0) out highp float depth;

void main()
{
	vec4 vpos = uniformBuffer.ndcMat * in_pos;
#if DIV_POS_Z == 1
	vpos /= vpos.z;
	vpos.z = vpos.w;
	depth = vpos.w;
#else
	depth = vpos.z;
	vpos.w = 1.0;
	vpos.z = 0.0;
#endif
	gl_Position = vpos;
}
