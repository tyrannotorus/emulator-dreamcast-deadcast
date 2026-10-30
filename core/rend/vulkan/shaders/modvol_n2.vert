#version 450
// Deadcast: Naomi 2's modifier volumes' vertex shader, compiled to SPIR-V by the build
layout (std140, set = 0, binding = 0) uniform VertexShaderUniforms
{
	mat4 ndcMat;
} uniformBuffer;

layout (std140, set = 1, binding = 2) uniform N2VertexShaderUniforms
{
	mat4 mvMat;
	mat4 normalMat;
	mat4 projMat;
	ivec2 envMapping;
	int bumpMapping;
	int polyNumber;

	vec2 glossCoef;
	ivec2 constantColor;
} n2Uniform;

layout (location = 0) in vec4 in_pos;
layout (location = 0) out highp float depth;

void wDivide(inout vec4 vpos)
{
	vpos = vec4(vpos.xy / vpos.w, 1.0 / vpos.w, 1.0);
	vpos = uniformBuffer.ndcMat * vpos;
	depth = vpos.z;
	vpos.w = 1.0;
	vpos.z = 0.0;
}

void main()
{
	vec4 vpos = n2Uniform.mvMat * in_pos;
	vpos.z = min(vpos.z, -0.001);
	vpos = n2Uniform.projMat * vpos;
	wDivide(vpos);

	gl_Position = vpos;
}
