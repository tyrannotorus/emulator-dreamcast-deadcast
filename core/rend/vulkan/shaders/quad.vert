#version 450
// Deadcast: the quads' vertex shader (the frame to the screen, the overlays), compiled to SPIR-V by the
// build once for each ROTATE (0, 1)
layout (location = 0) in vec3 in_pos;
layout (location = 1) in vec2 in_uv;

layout (location = 0) out vec2 outUV;

void main()
{
#if ROTATE == 0
	gl_Position = vec4(in_pos, 1.0);
#else
	gl_Position = vec4(in_pos.y, -in_pos.x, in_pos.z, 1.0);
#endif
	outUV = in_uv;
}
