#version 450
// Deadcast: the quads' fragment shader, compiled to SPIR-V by the build once for each IGNORE_TEX_ALPHA (0, 1)
layout (set = 0, binding = 0) uniform sampler2D tex;

layout (push_constant) uniform pushBlock
{
	vec4 color;
} pushConstants;

layout (location = 0) in vec2 inUV;
layout (location = 0) out vec4 FragColor;

void main()
{
#if IGNORE_TEX_ALPHA == 1
	FragColor.rgb = pushConstants.color.rgb * texture(tex, inUV).rgb;
	FragColor.a = pushConstants.color.a;
#else
	FragColor = pushConstants.color * texture(tex, inUV);
#endif
}
