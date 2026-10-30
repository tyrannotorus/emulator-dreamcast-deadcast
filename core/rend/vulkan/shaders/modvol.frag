#version 450
// Deadcast: the modifier volumes' fragment shader, compiled to SPIR-V by the build once for each DIV_POS_Z (0, 1)
precision highp float;
precision highp int;

layout (location = 0) in highp float depth;
layout (location = 0) out vec4 FragColor;

layout (push_constant) uniform pushBlock
{
	float sp_ShaderColor;
} pushConstants;

void main()
{
#if DIV_POS_Z == 1
	highp float w = 100000.0 / depth;
#else
	highp float w = 100000.0 * depth;
#endif
	gl_FragDepth = log2(1.0 + max(w, -0.999999)) / 34.0;
	FragColor = vec4(0.0, 0.0, 0.0, pushConstants.sp_ShaderColor);
}
