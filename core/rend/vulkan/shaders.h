/*
 *  Created on: Oct 3, 2019

	Copyright 2019 flyinghead

	This file is part of Flycast.

    Flycast is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    Flycast is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with Flycast.  If not, see <https://www.gnu.org/licenses/>.
*/
#pragma once
#include "vulkan.h"

#include <glm/glm.hpp>
#include <array>

struct VertexShaderParams
{
	bool gouraud;
	bool naomi2;
	bool divPosZ;
	bool texture;

	u32 hash() { return (u32)gouraud | ((u32)naomi2 << 1) | ((u32)divPosZ << 2) | ((u32)texture << 3); }
};

// alpha test, clip test, use alpha, texture, ignore alpha, shader instr, offset, fog, gouraud, bump, clamp, trilinear
struct FragmentShaderParams
{
	bool alphaTest;
	bool insideClipTest;
	bool useAlpha;
	bool texture;
	bool ignoreTexAlpha;
	int shaderInstr;
	bool offset;
	int fog;
	bool gouraud;
	bool bumpmap;
	bool clamping;
	bool trilinear;
	int palette;
	bool divPosZ;
	bool dithering;

	u32 hash()
	{
		return ((u32)alphaTest) | ((u32)insideClipTest << 1) | ((u32)useAlpha << 2)
			| ((u32)texture << 3) | ((u32)ignoreTexAlpha << 4) | (shaderInstr << 5)
			| ((u32)offset << 7) | ((u32)fog << 8) | ((u32)gouraud << 10)
			| ((u32)bumpmap << 11) | ((u32)clamping << 12) | ((u32)trilinear << 13)
			| ((u32)palette << 14) | ((u32)divPosZ << 16) | ((u32)dithering << 17);
	}
};

struct ModVolShaderParams
{
	bool naomi2;
	bool divPosZ;

	u32 hash() { return (u32)naomi2 | ((u32)divPosZ << 1); }
};

// std140 alignment required
struct VertexShaderUniforms
{
	glm::mat4 ndcMat;
};

// std140 alignment required
struct FragmentShaderUniforms
{
	float colorClampMin[4];
	float colorClampMax[4];
	float sp_FOG_COL_RAM[4];	// Only using 3 elements but easier for std140
	float sp_FOG_COL_VERT[4];	// same comment
	float ditherDivisor[4];
	float cp_AlphaTestValue;
	float sp_FOG_DENSITY;
};

// std140 alignment required
struct N2VertexShaderUniforms
{
	glm::mat4 mvMat;
	glm::mat4 normalMat;
	glm::mat4 projMat;
	int envMapping[2];
	int bumpMapping;
	int polyNumber;

	float glossCoef[2];
	int constantColor[2];
};

// Deadcast: the specialization of the polygons' fragment shader for one FragmentShaderParams (poly.frag's
// constants, in its order). info points into the struct: used where it was filled.
struct FragmentSpecialization
{
	std::array<u32, 14> values;
	vk::SpecializationInfo info;
};

// Deadcast: the shaders, compiled to SPIR-V by the build (shaders/*, core/rend/vulkan/CMakeLists.txt) and
// made into modules the first time a pipeline needs them
class ShaderManager
{
public:
	vk::ShaderModule GetVertexShader(const VertexShaderParams& params);
	// the module for params, and its specialization in spec
	vk::ShaderModule GetFragmentShader(const FragmentShaderParams& params, FragmentSpecialization& spec);
	vk::ShaderModule GetModVolVertexShader(const ModVolShaderParams& params);
	vk::ShaderModule GetModVolShader(bool divPosZ);
	vk::ShaderModule GetQuadVertexShader(bool rotate = false);
	vk::ShaderModule GetQuadFragmentShader(bool ignoreTexAlpha);

	void term()
	{
		for (auto& module : modules)
			module.reset();
	}

private:
	vk::ShaderModule get(int index);

	std::array<vk::UniqueShaderModule, 19> modules;
};
