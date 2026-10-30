/*
	Copyright 2022 flyinghead

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
#include "ta_ctx.h"
#include "pvr_mem.h"
#include <algorithm>
#include <cmath>
#include <glm/glm.hpp>
#include <glm/gtc/type_ptr.hpp>

//
// Check if a vertex has NaN or huge x,y,z values
//
static bool is_vertex_inf(const Vertex& vtx)
{
	// manic panic ghosts needs 1.0e25f for x and y
	return std::isnan(vtx.x) || fabsf(vtx.x) > 1e25f
			|| std::isnan(vtx.y) || fabsf(vtx.y) > 1e25f
			|| std::isnan(vtx.z) || vtx.z > 3.4e37f;
}

struct IndexTrig
{
	IndexTrig() = default;
	IndexTrig(u32 pid, u32 v0, u32 v1, u32 v2) : pid(pid), z(0) {
		vid[0] = v0;
		vid[1] = v1;
		vid[2] = v2;
	}

	u32 vid[3];
	u32 pid;
	f32 z;
};

static float minZ(const Vertex *v, const u32 *mod)
{
	return std::min(std::min(v[mod[0]].z, v[mod[1]].z), v[mod[2]].z);
}

static bool operator<(const IndexTrig& left, const IndexTrig& right)
{
	return left.z < right.z;
}

static float getProjectedZ(const Vertex *v, const float *mat)
{
	// -1 / z
	return -1 / (mat[2] * v->x + mat[1 * 4 + 2] * v->y + mat[2 * 4 + 2] * v->z + mat[3 * 4 + 2]);
}

void sortTriangles(rend_context& ctx, RenderPass& pass, const RenderPass& previousPass)
{
	int first = previousPass.tr_count;
	int count = pass.tr_count - first;
	if (count == 0)
		return;

	const PolyParam * const pp_base = &ctx.global_param_tr[first];
	const PolyParam * const pp_end = pp_base + count;

	//make lists of all triangles, with their pid and vid
	static std::vector<IndexTrig> triangleList;

	int vtx_count = ctx.verts.size() - pp_base->first;
	triangleList.reserve(vtx_count);
	triangleList.clear();

	for (const PolyParam *pp = pp_base; pp != pp_end; pp++)
	{
		if (pp->count < 3)
			continue;

		const Vertex *v0 = &ctx.verts[pp->first];
		const Vertex *v1 = &ctx.verts[pp->first + 1];
		float z0 = 0, z1 = 0;

		if (pp->isNaomi2())
		{
			z0 = getProjectedZ(v0, ctx.matrices[pp->mvMatrix].mat);
			z1 = getProjectedZ(v1, ctx.matrices[pp->mvMatrix].mat);
		}
		else
		{
			if (is_vertex_inf(*v0))
				v0 = nullptr;
			if (is_vertex_inf(*v1))
				v1 = nullptr;
		}
		for (u32 i = 2; i < pp->count; i++)
		{
			const Vertex *v2 = &ctx.verts[pp->first + i];
			if (!pp->isNaomi2() && is_vertex_inf(*v2))
				v2 = nullptr;
			if (v0 != nullptr && v1 != nullptr && v2 != nullptr)
			{
				triangleList.emplace_back((u32)(pp - pp_base),
						(u32)(v0 - &ctx.verts[0]), (u32)(v1 - &ctx.verts[0]), (u32)(v2 - &ctx.verts[0]));
				if (pp->isNaomi2())
				{
					float z2 = getProjectedZ(v2, ctx.matrices[pp->mvMatrix].mat);
					triangleList.back().z = std::min(z0, std::min(z1, z2));
					z0 = z1;
					z1 = z2;
				}
				else
				{
					triangleList.back().z = minZ(&ctx.verts[0], triangleList.back().vid);
				}
			}
			if (i & 1)
				v1 = v2;
			else
				v0 = v2;
		}
	}

	//sort them
	std::stable_sort(triangleList.begin(), triangleList.end());

	//Merge pids/draw cmds if two different pids are actually equal
	for (size_t k = 1; k < triangleList.size(); k++)
		if (triangleList[k].pid != triangleList[k - 1].pid)
		{
			const PolyParam& curPoly = pp_base[triangleList[k].pid];
			const PolyParam& prevPoly = pp_base[triangleList[k - 1].pid];
			if (curPoly.equivalentIgnoreCullingDirection(prevPoly)
					&& (curPoly.isp.CullMode < 2 || curPoly.isp.CullMode == prevPoly.isp.CullMode))
				triangleList[k].pid = triangleList[k - 1].pid;
		}

	//re-assemble them into drawing commands

	int idx = -1;
	int idxSize = ctx.idx.size();

	for (size_t i = 0; i < triangleList.size(); i++)
	{
		int pid = triangleList[i].pid;
		u32* midx = triangleList[i].vid;

		ctx.idx.emplace_back(midx[0]);
		ctx.idx.emplace_back(midx[1]);
		ctx.idx.emplace_back(midx[2]);

		if (idx != pid)
		{
			SortedTriangle cur = { (u32)(&pp_base[pid] - &ctx.global_param_tr[0]), (u32)(idxSize + i * 3), 0 };

			if (idx != -1)
			{
				SortedTriangle& last = ctx.sortedTriangles.back();
				last.count = cur.first - last.first;
			}

			ctx.sortedTriangles.push_back(cur);
			idx = pid;
		}
	}

	if (!triangleList.empty())
	{
		SortedTriangle& last = ctx.sortedTriangles.back();
		last.count = idxSize + triangleList.size() * 3 - last.first;
	}
	else
	{
		// Add a dummy one to signal we're using sorted triangles
		ctx.sortedTriangles.push_back({ (u32)(&pp_base[0] - &ctx.global_param_tr[0]), 0, 0});
	}
	pass.sorted_tr_count = ctx.sortedTriangles.size();

#if PRINT_SORT_STATS
	printf("Reassembled into %d from %d\n", (int)ctx.sortedTriangles.size(), pp_end - pp_base);
#endif
}

static bool operator<(const PolyParam& left, const PolyParam& right)
{
	return left.zvZ < right.zvZ;
}

// Deadcast: the PowerVR sorts translucent polygons per pixel. Sorting strips by their farthest point
// misplaces a large receding plane (NHL 2K2's ice), drawing it over what lies beneath it. So the
// largest translucent plane receding from the camera goes after the strips wholly behind it on the
// screen and before the others; strips on the plane keep their order with it.
namespace
{
struct ScreenPlane
{
	double a = 0, b = 0, c = 0;	// 1/w = a x + b y + c

	double z(double x, double y) const {
		return a * x + b * y + c;
	}
	// from the three vertices of v spanning the largest triangle on the screen
	bool fit(const Vertex *v, u32 count)
	{
		double best = 0;
		const Vertex *p0 = &v[0], *p1 = nullptr, *p2 = nullptr;
		for (u32 i = 1; i < count; i++)
			for (u32 j = i + 1; j < count; j++)
			{
				double area = std::abs(((double)v[i].x - p0->x) * ((double)v[j].y - p0->y) - ((double)v[j].x - p0->x) * ((double)v[i].y - p0->y));
				if (area > best)
				{
					best = area;
					p1 = &v[i];
					p2 = &v[j];
				}
			}
		if (best < 1.0)
			return false;
		// solve [x y 1] [a b c]' = z for the three points
		const double x0 = p0->x, y0 = p0->y, z0 = p0->z;
		const double x1 = p1->x - x0, y1 = p1->y - y0, z1 = p1->z - z0;
		const double x2 = p2->x - x0, y2 = p2->y - y0, z2 = p2->z - z0;
		const double det = x1 * y2 - x2 * y1;
		if (std::abs(det) < 1e-9)
			return false;
		a = (z1 * y2 - z2 * y1) / det;
		b = (x1 * z2 - x2 * z1) / det;
		c = z0 - a * x0 - b * y0;
		return true;
	}
	// where the vertex is, relative to the plane: -1 behind (farther), 0 on it, 1 in front
	int side(const Vertex& v) const
	{
		const double pz = z(v.x, v.y);
		const double tolerance = 1e-3 * std::max(std::abs(pz), std::abs((double)v.z)) + 1e-7;
		if (v.z < pz - tolerance)
			return -1;
		if (v.z > pz + tolerance)
			return 1;
		return 0;
	}
};

double screenArea(const Vertex *v, u32 count)
{
	float x0 = 1e30f, x1 = -1e30f, y0 = 1e30f, y1 = -1e30f;
	for (u32 i = 0; i < count; i++)
	{
		x0 = std::min(x0, v[i].x);
		x1 = std::max(x1, v[i].x);
		y0 = std::min(y0, v[i].y);
		y1 = std::max(y1, v[i].y);
	}
	const double w = std::min(x1, 640.f) - std::max(x0, 0.f);
	const double h = std::min(y1, 480.f) - std::max(y0, 0.f);
	return w > 0 && h > 0 ? w * h : 0;
}
}

static void placeLargestPlane(PolyParam *first, PolyParam *end, rend_context& ctx)
{
	// the largest strip on the screen that is a plane going away from the camera (1/w a quarter more
	// at its nearest than at its farthest), as large as a quarter of the screen
	ScreenPlane plane;
	double largest = 640 * 480 / 4;
	bool found = false;
	for (PolyParam *pp = first; pp != end; pp++)
	{
		if (pp->count < 3 || pp->isNaomi2())
			continue;
		const Vertex *v = &ctx.verts[pp->first];
		const double area = screenArea(v, pp->count);
		if (area <= largest)
			continue;
		float nearest = v[0].z, farthest = v[0].z;
		for (u32 i = 1; i < pp->count; i++)
		{
			nearest = std::max(nearest, v[i].z);
			farthest = std::min(farthest, v[i].z);
		}
		if (!(farthest > 0 && nearest > farthest * 1.25f))
			continue;
		ScreenPlane candidate;
		if (!candidate.fit(v, pp->count))
			continue;
		bool flat = true;
		for (u32 i = 0; i < pp->count && flat; i++)
			flat = candidate.side(v[i]) == 0;
		if (!flat)
			continue;
		plane = candidate;
		largest = area;
		found = true;
	}
	if (!found)
		return;
	// behind it: every vertex behind; on it: every vertex on it; the rest is in front
	auto where = [&](const PolyParam& pp) {
		if (pp.count == 0 || pp.isNaomi2())
			return 1;
		const Vertex *v = &ctx.verts[pp.first];
		int lowest = 1, highest = -1;
		for (u32 i = 0; i < pp.count; i++)
		{
			const int s = plane.side(v[i]);
			lowest = std::min(lowest, s);
			highest = std::max(highest, s);
		}
		return highest < 0 ? -1 : highest == 0 && lowest == 0 ? 0 : 1;
	};
	PolyParam *middle = std::stable_partition(first, end, [&](const PolyParam& pp) { return where(pp) < 0; });
	std::stable_partition(middle, end, [&](const PolyParam& pp) { return where(pp) == 0; });
}

void sortPolyParams(std::vector<PolyParam>& polys, int first, int end, rend_context& ctx)
{
	if (end - first <= 1)
		return;

	PolyParam * const pp_end = polys.data() + end;

	for (PolyParam *pp = &polys[first]; pp != pp_end; pp++)
	{
		if (pp->count < 3)
		{
			pp->zvZ = 0;
		}
		else
		{
			Vertex *vtx = &ctx.verts[pp->first];
			Vertex *vtx_end = vtx + pp->count;

			if (pp->isNaomi2())
			{
				glm::mat4 mvMat = glm::make_mat4(ctx.matrices[pp->mvMatrix].mat);
				glm::vec3 min{ 1e38f, 1e38f, 1e38f };
				glm::vec3 max{ -1e38f, -1e38f, -1e38f };
				while (vtx != vtx_end)
				{
					glm::vec3 pos{ vtx->x, vtx->y, vtx->z };
					min = glm::min(min, pos);
					max = glm::max(max, pos);
					vtx++;
				}
				glm::vec4 center((min + max) / 2.f, 1);
				glm::vec4 extents(max - glm::vec3(center), 0);
				// transform
				center = mvMat * center;
				glm::vec3 extentX = mvMat * glm::vec4(extents.x, 0, 0, 0);
				glm::vec3 extentY = mvMat * glm::vec4(0, extents.y, 0, 0);
				glm::vec3 extentZ = mvMat * glm::vec4(0, 0, extents.z, 0);
				// new AA extents
				glm::vec3 newExtent = glm::abs(extentX) + glm::abs(extentY) + glm::abs(extentZ);

				min = glm::vec3(center) - newExtent;
				max = glm::vec3(center) + newExtent;

				// project
				pp->zvZ = -1 / std::min(min.z, max.z);
			}
			else
			{
				u32 zv = 0xFFFFFFFF;
				while (vtx != vtx_end)
				{
					zv = std::min(zv, (u32&)vtx->z);
					vtx++;
				}

				pp->zvZ = (f32&)zv;
			}
		}
	}

	std::stable_sort(&polys[first], pp_end);
	placeLargestPlane(&polys[first], pp_end, ctx);
}

void getRegionTileAddrAndSize(u32& address, u32& size)
{
	address = REGION_BASE;
	const bool type1_tile = ((FPU_PARAM_CFG >> 21) & 1) == 0;
	size = (type1_tile ? 5 : 6) * 4;
	bool empty_first_region = true;
	for (int i = type1_tile ? 4 : 5; i > 0; i--)
		if ((pvr_read32p<u32>(address + i * 4) & 0x80000000) == 0)
		{
			empty_first_region = false;
			break;
		}
	if (empty_first_region)
		address += size;
	RegionArrayTile tile;
	tile.full = pvr_read32p<u32>(address);
	if (tile.PreSort)
		// Windows CE weirdness
		size = 6 * 4;
}

int getTAContextAddresses(u32 *addresses)
{
	u32 addr;
	u32 tile_size;
	getRegionTileAddrAndSize(addr, tile_size);

	RegionArrayTile tile;
	tile.full = pvr_read32p<u32>(addr);
	u32 x = tile.X;
	u32 y = tile.Y;
	u32 count = 0;
	do {
		tile.full = pvr_read32p<u32>(addr);
		if (tile.X != x || tile.Y != y)
			break;
		// Try the opaque pointer
		u32 opbAddr = pvr_read32p<u32>(addr + 4);
		if (opbAddr & 0x80000000)
		{
			// Try the translucent pointer
			opbAddr = pvr_read32p<u32>(addr + 12);
			if (opbAddr & 0x80000000)
			{
				// Try the punch-through pointer
				if (tile_size >= 24)
					opbAddr = pvr_read32p<u32>(addr + 20);
				if (opbAddr & 0x80000000)
				{
					INFO_LOG(PVR, "Can't find any non-null OPB for pass %d", count);
					break;
				}
			}
		}
		addresses[count++] = pvr_read32p<u32>(opbAddr);
		addr += tile_size;
	} while (!tile.LastRegion && count < MAX_PASSES);

	return count;
}

void fix_texture_bleeding(const std::vector<PolyParam>& polys, int first, int end, rend_context& ctx)
{
	auto pp_end = polys.begin() + end;
	for (auto pp = polys.begin() + first; pp != pp_end; ++pp)
	{
		if (!pp->pcw.Texture || pp->count < 3 || pp->isNaomi2())
			continue;
		// Find polygons that are facing the camera (constant z)
		// and only use 0 and 1 for U and V (some tolerance around 1 for SA2)
		// then apply a half-pixel correction on U and V.
		const u32 last = pp->first + pp->count;
		bool need_fixing = true;
		float z = 0.f;
		for (u32 idx = pp->first; idx < last && need_fixing; idx++)
		{
			Vertex& vtx = ctx.verts[idx];

			if (vtx.u != 0.f && (vtx.u <= 0.995f || vtx.u > 1.f))
				need_fixing = false;
			else if (vtx.v != 0.f && (vtx.v <= 0.995f || vtx.v > 1.f))
				need_fixing = false;
			else if (idx == pp->first)
				z = vtx.z;
			else if (z != vtx.z)
				need_fixing = false;
		}
		if (!need_fixing)
			continue;
		u32 tex_width = 8 << pp->tsp.TexU;
		u32 tex_height = 8 << pp->tsp.TexV;
		for (u32 idx = pp->first; idx < last; idx++)
		{
			Vertex& vtx = ctx.verts[idx];
			if (vtx.u > 0.995f)
				vtx.u = 1.f;
			vtx.u = (0.5f + vtx.u * (tex_width - 1)) / tex_width;
			if (vtx.v > 0.995f)
				vtx.v = 1.f;
			vtx.v = (0.5f + vtx.v * (tex_height - 1)) / tex_height;
		}
	}
}

//
// Create the vertex index, eliminating invalid vertices and merging strips when possible.
// Use primitive restart when merging strips.
//
void makePrimRestartIndex(std::vector<PolyParam>& polys, int first, int end, bool merge, rend_context& ctx)
{
	if (first >= (int)polys.size())
		return;
	PolyParam *last_poly = nullptr;
	const PolyParam *end_poly = polys.data() + end;
	for (PolyParam *poly = &polys[first]; poly != end_poly; poly++)
	{
		int first_index;
		bool dupe_next_vtx = false;
		if (merge
				&& last_poly != nullptr
				&& last_poly->count != 0
				&& poly->equivalentIgnoreCullingDirection(*last_poly))
		{
			ctx.idx.push_back(~0);
			dupe_next_vtx = poly->isp.CullMode >= 2 && poly->isp.CullMode != last_poly->isp.CullMode;
			first_index = last_poly->first;
		}
		else
		{
			last_poly = poly;
			first_index = ctx.idx.size();
		}
		int last_good_vtx = -1;
		for (u32 i = 0; i < poly->count; i++)
		{
			const Vertex& vtx = ctx.verts[poly->first + i];
			if (!poly->isNaomi2() && is_vertex_inf(vtx))
			{
				bool odd = i & 1;
				while (i < poly->count - 1)
				{
					odd = !odd;
					const Vertex& next_vtx = ctx.verts[poly->first + i + 1];
					if (!is_vertex_inf(next_vtx))
					{
						if (poly->count - (i + 1) < 3)
							// skip remaining incomplete triangle
							i = poly->count - 1;
						else
						{
							if (last_good_vtx >= 0)
								// reset the strip
								ctx.idx.push_back(~0);
							if (odd && poly->isp.CullMode >= 2)
								// repeat next vertex to get culling right
								dupe_next_vtx = true;
						}
						break;
					}
					i++;
				}
			}
			else
			{
				last_good_vtx = poly->first + i;
				if (dupe_next_vtx)
				{
					ctx.idx.push_back(last_good_vtx);
					dupe_next_vtx = false;
				}
				ctx.idx.push_back(last_good_vtx);
			}
		}
		if (last_poly == poly)
		{
			poly->first = first_index;
			poly->count = ctx.idx.size() - first_index;
		}
		else
		{
			last_poly->count = ctx.idx.size() - last_poly->first;
			poly->count = 0;
		}
	}
}

//
// Create the vertex index, eliminating invalid vertices and merging strips when possible.
// Use degenerate triangles to link strips.
//
void makeIndex(std::vector<PolyParam>& polys, int first, int end, bool merge, rend_context& ctx)
{
	if (first >= (int)polys.size())
		return;
	PolyParam *last_poly = nullptr;
	const PolyParam *end_poly = polys.data() + end;
	bool cullingReversed = false;
	for (PolyParam *poly = &polys[first]; poly != end_poly; poly++)
	{
		int first_index;
		bool dupe_next_vtx = false;
		if (merge
				&& last_poly != nullptr
				&& last_poly->count != 0
				&& poly->equivalentIgnoreCullingDirection(*last_poly))
		{
			const u32 last_vtx = ctx.idx[last_poly->first + last_poly->count - 1];
			ctx.idx.push_back(last_vtx);
			if (poly->isp.CullMode < 2 || poly->isp.CullMode == last_poly->isp.CullMode)
			{
				if (cullingReversed)
					ctx.idx.push_back(last_vtx);
				cullingReversed = false;
			}
			else
			{
				if (!cullingReversed)
					ctx.idx.push_back(last_vtx);
				cullingReversed = true;
			}
			dupe_next_vtx = true;
			first_index = last_poly->first;
		}
		else
		{
			last_poly = poly;
			first_index = ctx.idx.size();
			cullingReversed = false;
		}
		int last_good_vtx = -1;
		for (u32 i = 0; i < poly->count; i++)
		{
			const Vertex& vtx = ctx.verts[poly->first + i];
			if (!poly->isNaomi2() && is_vertex_inf(vtx))
			{
				while (i < poly->count - 1)
				{
					const Vertex& next_vtx = ctx.verts[poly->first + i + 1];
					if (!is_vertex_inf(next_vtx))
					{
						// repeat last and next vertices to link strips
						if (last_good_vtx >= 0)
						{
							verify(!dupe_next_vtx);
							ctx.idx.push_back(last_good_vtx);
							dupe_next_vtx = true;
						}
						break;
					}
					i++;
				}
			}
			else
			{
				last_good_vtx = poly->first + i;
				if (dupe_next_vtx)
				{
					ctx.idx.push_back(last_good_vtx);
					dupe_next_vtx = false;
				}
				const u32 count = ctx.idx.size() - first_index;
				if (((i ^ count) & 1) ^ cullingReversed)
					ctx.idx.push_back(last_good_vtx);
				ctx.idx.push_back(last_good_vtx);
			}
		}
		if (last_poly == poly)
		{
			poly->first = first_index;
			poly->count = ctx.idx.size() - first_index;
		}
		else
		{
			last_poly->count = ctx.idx.size() - last_poly->first;
			poly->count = 0;
		}
	}
}

