#version 450
// Deadcast: the polygons' fragment shader, compiled to SPIR-V by the build (core/rend/vulkan/CMakeLists.txt)
// once with pp_Gouraud=0 (flat) and once with pp_Gouraud=1: the rest of a polygon's shading is in
// specialization constants (FragmentShaderParams, shaders.h, in this order), which the driver folds
// into each pipeline as the preprocessor did.
#if pp_Gouraud == 0
#define INTERPOLATION flat
#else
#define INTERPOLATION
#endif

layout (constant_id = 0) const bool cp_AlphaTest = false;
layout (constant_id = 1) const bool pp_ClipInside = false;
layout (constant_id = 2) const bool pp_UseAlpha = false;
layout (constant_id = 3) const bool pp_Texture = false;
layout (constant_id = 4) const bool pp_IgnoreTexA = false;
layout (constant_id = 5) const int pp_ShadInstr = 0;
layout (constant_id = 6) const bool pp_Offset = false;
layout (constant_id = 7) const int pp_FogCtrl = 0;
layout (constant_id = 8) const bool pp_BumpMap = false;
layout (constant_id = 9) const bool ColorClamping = false;
layout (constant_id = 10) const bool pp_TriLinear = false;
layout (constant_id = 11) const int pp_Palette = 0;
layout (constant_id = 12) const bool DIV_POS_Z = false;
layout (constant_id = 13) const bool DITHERING = false;

precision highp float;
precision highp int;

layout (location = 0) out vec4 FragColor;

layout (std140, set = 0, binding = 1) uniform FragmentShaderUniforms
{
	vec4 colorClampMin;
	vec4 colorClampMax;
	vec4 sp_FOG_COL_RAM;
	vec4 sp_FOG_COL_VERT;
	vec4 ditherDivisor;
	float cp_AlphaTestValue;
	float sp_FOG_DENSITY;
} uniformBuffer;

layout (push_constant) uniform pushBlock
{
	vec4 clipTest;
	float trilinearAlpha;
	float palette_index;
} pushConstants;

layout (set = 1, binding = 0) uniform sampler2D tex;
layout (set = 0, binding = 2) uniform sampler2D fog_table;
layout (set = 0, binding = 3) uniform sampler2D palette;

layout (location = 0) INTERPOLATION in highp vec4 vtx_base;
layout (location = 1) INTERPOLATION in highp vec4 vtx_offs;
layout (location = 2) in highp vec3 vtx_uv;

#define PI 3.1415926

float fog_mode2(float w)
{
	float z = clamp(DIV_POS_Z ? uniformBuffer.sp_FOG_DENSITY / w : uniformBuffer.sp_FOG_DENSITY * w, 1.0, 255.9999);
	float exp = floor(log2(z));
	float m = z * 16.0 / pow(2.0, exp) - 16.0;
	float idx = floor(m) + exp * 16.0 + 0.5;
	vec4 fog_coef = texture(fog_table, vec2(idx / 128.0, 0.75 - (m - floor(m)) / 2.0));
	return fog_coef.r;
}

vec4 colorClamp(vec4 col)
{
	if (ColorClamping)
		return clamp(col, uniformBuffer.colorClampMin, uniformBuffer.colorClampMax);
	return col;
}

vec4 getPaletteEntry(float colIdx)
{
	vec2 c = vec2(colIdx * 255.0 / 1023.0 + pushConstants.palette_index, 0.5);
	return texture(palette, c);
}

vec4 palettePixel(vec3 coords)
{
	if (DIV_POS_Z)
		return getPaletteEntry(texture(tex, coords.xy).r);
	return getPaletteEntry(textureProj(tex, coords).r);
}

vec4 palettePixelBilinear(vec3 coords)
{
	if (!DIV_POS_Z)
		coords.xy /= coords.z;
	vec2 texSize = vec2(textureSize(tex, 0));
	vec2 pixCoord = coords.xy * texSize - 0.5;			// coordinates of top left pixel
	vec2 originPixCoord = floor(pixCoord);

	vec2 sampleUV = (originPixCoord + 0.5) / texSize;	// UV coordinates of center of top left pixel

	// Sample from all surrounding texels
	vec4 c00 = getPaletteEntry(texture(tex, sampleUV).r);
	vec4 c01 = getPaletteEntry(textureOffset(tex, sampleUV, ivec2(0, 1)).r);
	vec4 c11 = getPaletteEntry(textureOffset(tex, sampleUV, ivec2(1, 1)).r);
	vec4 c10 = getPaletteEntry(textureOffset(tex, sampleUV, ivec2(1, 0)).r);

	vec2 weight = pixCoord - originPixCoord;

	// Bi-linear mixing
	vec4 temp0 = mix(c00, c10, weight.x);
	vec4 temp1 = mix(c01, c11, weight.x);
	return mix(temp0, temp1, weight.y);
}

void main()
{
	// Clip inside the box
	if (pp_ClipInside)
	{
		if (gl_FragCoord.x >= pushConstants.clipTest.x && gl_FragCoord.x <= pushConstants.clipTest.z
				&& gl_FragCoord.y >= pushConstants.clipTest.y && gl_FragCoord.y <= pushConstants.clipTest.w)
			discard;
	}

	highp vec4 color = vtx_base;
	highp vec4 offset = vtx_offs;
#if pp_Gouraud == 1
	if (!DIV_POS_Z)
	{
		color /= vtx_uv.z;
		offset /= vtx_uv.z;
	}
#endif
	if (!pp_UseAlpha)
		color.a = 1.0;
	if (pp_FogCtrl == 3)
		color = vec4(uniformBuffer.sp_FOG_COL_RAM.rgb, fog_mode2(vtx_uv.z));
	if (pp_Texture)
	{
		vec4 texcol;
		if (pp_Palette == 0)
			texcol = DIV_POS_Z ? texture(tex, vtx_uv.xy) : textureProj(tex, vtx_uv);
		else if (pp_Palette == 1)
			texcol = palettePixel(vtx_uv);
		else
			texcol = palettePixelBilinear(vtx_uv);

		if (pp_BumpMap)
		{
			float s = PI / 2.0 * (texcol.a * 15.0 * 16.0 + texcol.r * 15.0) / 255.0;
			float r = 2.0 * PI * (texcol.g * 15.0 * 16.0 + texcol.b * 15.0) / 255.0;
			texcol.a = clamp(offset.a + offset.r * sin(s) + offset.g * cos(s) * cos(r - 2.0 * PI * offset.b), 0.0, 1.0);
			texcol.rgb = vec3(1.0, 1.0, 1.0);
		}
		else if (pp_IgnoreTexA)
			texcol.a = 1.0;

		if (pp_ShadInstr == 0)
			color = texcol;
		else if (pp_ShadInstr == 1)
		{
			color.rgb *= texcol.rgb;
			color.a = texcol.a;
		}
		else if (pp_ShadInstr == 2)
			color.rgb = mix(color.rgb, texcol.rgb, texcol.a);
		else
			color *= texcol;

		if (pp_Offset && !pp_BumpMap)
			color.rgb += offset.rgb;
	}

	color = colorClamp(color);

	if (pp_FogCtrl == 0)
		color.rgb = mix(color.rgb, uniformBuffer.sp_FOG_COL_RAM.rgb, fog_mode2(vtx_uv.z));
	if (pp_FogCtrl == 1 && pp_Offset && !pp_BumpMap)
		color.rgb = mix(color.rgb, uniformBuffer.sp_FOG_COL_VERT.rgb, offset.a);

	if (pp_TriLinear)
		color *= pushConstants.trilinearAlpha;

	if (cp_AlphaTest)
	{
		color.a = round(color.a * 255.0) / 255.0;
		if (uniformBuffer.cp_AlphaTestValue > color.a)
			discard;
		color.a = 1.0;
	}

	highp float w = DIV_POS_Z ? 100000.0 / vtx_uv.z : 100000.0 * vtx_uv.z;
	gl_FragDepth = log2(1.0 + max(w, -0.999999)) / 34.0;

	if (DITHERING)
	{
		float ditherTable[16] = float[](
			5., 13.,  7., 15.,
			9.,  1., 11.,  3.,
			6., 14.,  4., 12.,
			10., 2.,  8.,  0.
		);
		float r = ditherTable[int(mod(gl_FragCoord.y, 4.)) * 4 + int(mod(gl_FragCoord.x, 4.))];
		vec4 dv = vec4(r, r, r, 1.) / uniformBuffer.ditherDivisor;
		color = clamp(floor(color * 255. + dv) / 255., 0., 1.);
	}
	FragColor = color;
}
