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
#include "vulkan.h"
#include "shaders.h"
#include "vulkan_context.h"

#include <cstdint>

// Deadcast: the SPIR-V the build made of shaders/* (core/rend/vulkan/CMakeLists.txt)
#include "spv/poly_vert_g0_d0.h"
#include "spv/poly_vert_g0_d1.h"
#include "spv/poly_vert_g1_d0.h"
#include "spv/poly_vert_g1_d1.h"
#include "spv/poly_n2_vert_g0_t0.h"
#include "spv/poly_n2_vert_g0_t1.h"
#include "spv/poly_n2_vert_g1_t0.h"
#include "spv/poly_n2_vert_g1_t1.h"
#include "spv/poly_frag_g0.h"
#include "spv/poly_frag_g1.h"
#include "spv/modvol_vert_d0.h"
#include "spv/modvol_vert_d1.h"
#include "spv/modvol_n2_vert.h"
#include "spv/modvol_frag_d0.h"
#include "spv/modvol_frag_d1.h"
#include "spv/quad_vert_r0.h"
#include "spv/quad_vert_r1.h"
#include "spv/quad_frag_a0.h"
#include "spv/quad_frag_a1.h"

namespace
{
struct Spirv
{
	const uint32_t *code;
	size_t size;
};
#define SPV(name) Spirv{ spv_##name, sizeof(spv_##name) }
// in the order of ShaderManager::modules
const Spirv spirv[] = {
	SPV(poly_vert_g0_d0), SPV(poly_vert_g0_d1), SPV(poly_vert_g1_d0), SPV(poly_vert_g1_d1),
	SPV(poly_n2_vert_g0_t0), SPV(poly_n2_vert_g0_t1), SPV(poly_n2_vert_g1_t0), SPV(poly_n2_vert_g1_t1),
	SPV(poly_frag_g0), SPV(poly_frag_g1),
	SPV(modvol_vert_d0), SPV(modvol_vert_d1), SPV(modvol_n2_vert), SPV(modvol_frag_d0), SPV(modvol_frag_d1),
	SPV(quad_vert_r0), SPV(quad_vert_r1), SPV(quad_frag_a0), SPV(quad_frag_a1),
};
#undef SPV
enum { PolyVert = 0, PolyN2Vert = 4, PolyFrag = 8, ModVolVert = 10, ModVolN2Vert = 12, ModVolFrag = 13, QuadVert = 15, QuadFrag = 17 };

// poly.frag's specialization constants: ids 0 to 13, a 32-bit value each
const std::array<vk::SpecializationMapEntry, 14> fragmentMap = [] {
	std::array<vk::SpecializationMapEntry, 14> map;
	for (u32 i = 0; i < map.size(); i++)
		map[i] = vk::SpecializationMapEntry(i, i * sizeof(u32), sizeof(u32));
	return map;
}();
}
static_assert(std::size(spirv) == 19, "ShaderManager::modules");

vk::ShaderModule ShaderManager::get(int index)
{
	vk::UniqueShaderModule& module = modules[index];
	if (!module)
		module = VulkanContext::Instance()->GetDevice().createShaderModuleUnique(
				vk::ShaderModuleCreateInfo(vk::ShaderModuleCreateFlags(), spirv[index].size, spirv[index].code));
	return *module;
}

vk::ShaderModule ShaderManager::GetVertexShader(const VertexShaderParams& params)
{
	if (params.naomi2)
		return get(PolyN2Vert + (params.gouraud ? 2 : 0) + (params.texture ? 1 : 0));
	return get(PolyVert + (params.gouraud ? 2 : 0) + (params.divPosZ ? 1 : 0));
}

vk::ShaderModule ShaderManager::GetFragmentShader(const FragmentShaderParams& params, FragmentSpecialization& spec)
{
	spec.values = {
		params.alphaTest, params.insideClipTest, params.useAlpha, params.texture, params.ignoreTexAlpha,
		(u32)params.shaderInstr, params.offset, (u32)params.fog, params.bumpmap, params.clamping,
		params.trilinear, (u32)params.palette, params.divPosZ, params.dithering,
	};
	spec.info = vk::SpecializationInfo(fragmentMap.size(), fragmentMap.data(), sizeof(spec.values), spec.values.data());
	return get(PolyFrag + (params.gouraud ? 1 : 0));
}

vk::ShaderModule ShaderManager::GetModVolVertexShader(const ModVolShaderParams& params)
{
	if (params.naomi2)
		return get(ModVolN2Vert);
	return get(ModVolVert + (params.divPosZ ? 1 : 0));
}

vk::ShaderModule ShaderManager::GetModVolShader(bool divPosZ) {
	return get(ModVolFrag + (divPosZ ? 1 : 0));
}

vk::ShaderModule ShaderManager::GetQuadVertexShader(bool rotate) {
	return get(QuadVert + (rotate ? 1 : 0));
}

vk::ShaderModule ShaderManager::GetQuadFragmentShader(bool ignoreTexAlpha) {
	return get(QuadFrag + (ignoreTexAlpha ? 1 : 0));
}
