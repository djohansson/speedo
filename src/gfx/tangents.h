#pragma once

#include <gfx/shaders/capi.h>

#include <cstdint>
#include <span>
#include <vector>

namespace gfx::mesh
{

// MikkTSpace tangents for triangles (indices into vertices, 3 per triangle), from a texcoord set (0 or 1, with v down the
// image as the importers store it): a tangent per corner, with a vertex split wherever its corners' tangents differ (uv
// seams, mirrored halves). tangents are made perpendicular to their own vertex's normal, and degenerate corners get none
// (w = 0: the shader derives the frame). replaces vertices and rewrites indices; returns, per new vertex, the index of
// the vertex it was copied from (for data kept alongside the vertices), or nothing if MikkTSpace failed (then vertices and
// indices are left as they were).
[[nodiscard]] std::vector<uint32_t> GenerateTangents(
	std::vector<VertexP3fN3fTa4fT014fC4f>& vertices, std::span<uint32_t> indices, uint32_t texCoordSet);

} // namespace gfx::mesh
