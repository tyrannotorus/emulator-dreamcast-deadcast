// Deadcast: Naomi 2's lighting, included by poly_n2.vert
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

#define PI 3.1415926

#define LMODE_SINGLE_SIDED 0
#define LMODE_DOUBLE_SIDED 1
#define LMODE_DOUBLE_SIDED_WITH_TOLERANCE 2
#define LMODE_SPECIAL_EFFECT 3
#define LMODE_THIN_SURFACE 4
#define LMODE_BUMP_MAP 5

#define ROUTING_SPEC_TO_OFFSET 1
#define ROUTING_DIFF_TO_OFFSET 2
#define ROUTING_ATTENUATION 1	// not handled
#define ROUTING_FOG 2			// not handled
#define ROUTING_ALPHA 4
#define ROUTING_SUB 8

struct N2Light
{
	vec4 color;
	vec4 direction;	// For parallel/spot
	vec4 position;	// For spot/point

	int parallel;
	int routing;
	int dmode;
	int smode;

	ivec2 diffuse;
	ivec2 specular;

	float attnDistA;
	float attnDistB;
	float attnAngleA;	// For spot
	float attnAngleB;

	int distAttnMode;	// For spot/point
	int _pad1;
	int _pad2;
	int _pad3;
};

layout (std140, set = 1, binding = 3) uniform N2Lights
{
	N2Light lights[16];
	vec4 ambientBase[2];
	vec4 ambientOffset[2];
	ivec2 ambientMaterialBase;
	ivec2 ambientMaterialOffset;
	int lightCount;
	int useBaseOver;
	int bumpId0;
	int bumpId1;
} n2Lights;

void computeColors(inout vec4 baseCol, inout vec4 offsetCol, in int volIdx, in vec3 position, in vec3 normal)
{
	if (n2Uniform.constantColor[volIdx] == 1)
		return;
	vec3 diffuse = vec3(0.0);
	vec3 specular = vec3(0.0);
	float diffuseAlpha = 0.0;
	float specularAlpha = 0.0;
	vec3 reflectDir = reflect(normalize(position), normal);
	const float BASE_FACTOR = 2.0;

	for (int i = 0; i < n2Lights.lightCount; i++)
	{
		vec3 lightDir; // direction to the light
		vec3 lightColor = n2Lights.lights[i].color.rgb;
		if (n2Lights.lights[i].parallel == 1)
		{
			lightDir = normalize(n2Lights.lights[i].direction.xyz);
		}
		else
		{
			lightDir = normalize(n2Lights.lights[i].position.xyz - position);
			if (n2Lights.lights[i].attnDistA != 1.0 || n2Lights.lights[i].attnDistB != 0.0)
			{
				float distance = length(n2Lights.lights[i].position.xyz - position);
				if (n2Lights.lights[i].distAttnMode == 0)
					distance = 1.0 / distance;
				lightColor *= clamp(n2Lights.lights[i].attnDistB * distance + n2Lights.lights[i].attnDistA, 0.0, 1.0);
			}
			if (n2Lights.lights[i].attnAngleA != 1.0 || n2Lights.lights[i].attnAngleB != 0.0)
			{
				vec3 spotDir = n2Lights.lights[i].direction.xyz;
				float cosAngle = 1.0 - max(0.0, dot(lightDir, spotDir));
				lightColor *= clamp(cosAngle * n2Lights.lights[i].attnAngleB + n2Lights.lights[i].attnAngleA, 0.0, 1.0);
			}
		}
		if (n2Lights.lights[i].diffuse[volIdx] == 1)
		{
			float factor = (n2Lights.lights[i].routing & ROUTING_SUB) != 0 ? -BASE_FACTOR : BASE_FACTOR;
			if (n2Lights.lights[i].dmode == LMODE_SINGLE_SIDED)
				factor *= max(dot(normal, lightDir), 0.0);
			else if (n2Lights.lights[i].dmode == LMODE_DOUBLE_SIDED)
				factor *= abs(dot(normal, lightDir));

			if ((n2Lights.lights[i].routing & ROUTING_ALPHA) != 0)
				diffuseAlpha += lightColor.r * factor;
			else
			{
				if ((n2Lights.lights[i].routing & ROUTING_DIFF_TO_OFFSET) == 0)
					diffuse += lightColor * factor * baseCol.rgb;
				else
					specular += lightColor * factor * baseCol.rgb;
			}
		}
		if (n2Lights.lights[i].specular[volIdx] == 1)
		{
			float factor = (n2Lights.lights[i].routing & ROUTING_SUB) != 0 ? -BASE_FACTOR : BASE_FACTOR;
			if (n2Lights.lights[i].smode == LMODE_SINGLE_SIDED)
				factor *= clamp(pow(max(dot(lightDir, reflectDir), 0.0), n2Uniform.glossCoef[volIdx]), 0.0, 1.0);
			else if (n2Lights.lights[i].smode == LMODE_DOUBLE_SIDED)
				factor *= clamp(pow(abs(dot(lightDir, reflectDir)), n2Uniform.glossCoef[volIdx]), 0.0, 1.0);

			if ((n2Lights.lights[i].routing & ROUTING_ALPHA) != 0)
				specularAlpha += lightColor.r * factor;
			else
			{
				if ((n2Lights.lights[i].routing & ROUTING_SPEC_TO_OFFSET) == 0)
					diffuse += lightColor * factor * offsetCol.rgb;
				else
					specular += lightColor * factor * offsetCol.rgb;
			}
		}
	}
	// ambient light
	if (n2Lights.ambientMaterialBase[volIdx] == 1)
		diffuse += n2Lights.ambientBase[volIdx].rgb * baseCol.rgb;
	else
		diffuse += n2Lights.ambientBase[volIdx].rgb;
	if (n2Lights.ambientMaterialOffset[volIdx] == 1)
		specular += n2Lights.ambientOffset[volIdx].rgb * offsetCol.rgb;
	else
		specular += n2Lights.ambientOffset[volIdx].rgb;
	baseCol.rgb = diffuse;
	offsetCol.rgb = specular;

	baseCol.a += diffuseAlpha;
	offsetCol.a += specularAlpha;
	if (n2Lights.useBaseOver == 1)
	{
		vec4 overflow = max(baseCol - vec4(1.0), 0.0);
		offsetCol += overflow;
	}
	baseCol = clamp(baseCol, 0.0, 1.0);
	offsetCol = clamp(offsetCol, 0.0, 1.0);
}

void computeEnvMap(inout vec2 uv, in vec3 position, in vec3 normal)
{
	// Cheap env mapping
	uv += normal.xy / 2.0 + 0.5;
	uv = clamp(uv, 0.0, 1.0);
}
