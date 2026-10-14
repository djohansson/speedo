#include "tangents.h"

#include <core/profiling.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <utility>

#include <glm/glm.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <mikktspace.h>

namespace gfx::mesh
{

namespace detail
{

// what GenerateTangents gives MikkTSpace (as its user data): the triangles, and the tangents it gets back, per corner
struct MikkContext
{
	const std::vector<VertexP3fN3fTa4fT014fC4f>* vertices;
	std::span<uint32_t> indices;
	uint32_t texCoordSet;
	std::vector<glm::vec4> cornerTangents;
};

[[nodiscard]] const VertexP3fN3fTa4fT014fC4f& MikkVertex(const SMikkTSpaceContext* mikk, int face, int corner)
{
	const auto* context = static_cast<const MikkContext*>(mikk->m_pUserData);
	return (*context->vertices)[context->indices[(static_cast<size_t>(face) * 3) + static_cast<size_t>(corner)]];
}

} // namespace detail

std::vector<uint32_t> GenerateTangents(std::vector<VertexP3fN3fTa4fT014fC4f>& vertices, std::span<uint32_t> indices, uint32_t texCoordSet)
{
	using namespace detail;

	ZoneScopedN("gfx::mesh::GenerateTangents");

	MikkContext context{
		.vertices = &vertices,
		.indices = indices,
		.texCoordSet = std::min(texCoordSet, 1U),
		.cornerTangents = std::vector<glm::vec4>(indices.size())};

	SMikkTSpaceInterface callbacks{};
	callbacks.m_getNumFaces = [](const SMikkTSpaceContext* mikk)
	{ return static_cast<int>(static_cast<const MikkContext*>(mikk->m_pUserData)->indices.size() / 3); };
	callbacks.m_getNumVerticesOfFace = [](const SMikkTSpaceContext*, int) { return 3; };
	callbacks.m_getPosition = [](const SMikkTSpaceContext* mikk, float out[], int face, int corner) //NOLINT(modernize-avoid-c-arrays)
	{ std::copy_n(MikkVertex(mikk, face, corner).position, 3, out); };
	callbacks.m_getNormal = [](const SMikkTSpaceContext* mikk, float out[], int face, int corner) //NOLINT(modernize-avoid-c-arrays)
	{ std::copy_n(MikkVertex(mikk, face, corner).normal, 3, out); };
	callbacks.m_getTexCoord = [](const SMikkTSpaceContext* mikk, float out[], int face, int corner) //NOLINT(modernize-avoid-c-arrays)
	{
		// with v flipped back up the image, as the exporters' MikkTSpace sees it (the importers' v points down): so that
		// the bitangent, cross(normal, tangent) * w, points up the image, as gltf's tangents do
		const auto* context = static_cast<const MikkContext*>(mikk->m_pUserData);
		const auto* uv = &MikkVertex(mikk, face, corner).texCoord01[2 * context->texCoordSet];
		out[0] = uv[0];
		out[1] = 1.0F - uv[1];
	};
	callbacks.m_setTSpaceBasic = [](const SMikkTSpaceContext* mikk, const float tangent[], float sign, int face, int corner) //NOLINT(modernize-avoid-c-arrays)
	{
		auto* context = static_cast<MikkContext*>(mikk->m_pUserData);
		// a degenerate corner (no area, or no texcoord gradient) gets no tangent: w = 0, the shader derives the frame
		auto t = glm::make_vec3(tangent);
		auto length = glm::length(t);
		context->cornerTangents[(static_cast<size_t>(face) * 3) + static_cast<size_t>(corner)] =
			std::isfinite(length) && length > 1e-6F ? glm::vec4(t / length, sign < 0.0F ? -1.0F : 1.0F) : glm::vec4(0.0F);
	};
	SMikkTSpaceContext mikk{.m_pInterface = &callbacks, .m_pUserData = &context};
	if (indices.empty() || genTangSpaceDefault(&mikk) == 0)
		return {};

	// each vertex once per distinct tangent of its corners
	std::vector<VertexP3fN3fTa4fT014fC4f> result;
	std::vector<uint32_t> sources;
	std::vector<std::vector<std::pair<glm::vec4, uint32_t>>> copies(vertices.size());
	result.reserve(vertices.size());
	sources.reserve(vertices.size());
	for (size_t cornerIt = 0; cornerIt < indices.size(); cornerIt++)
	{
		auto index = indices[cornerIt];
		// perpendicular to the vertex's own normal: a corner of a degenerate triangle gets a neighbor's tangent, whose
		// normal can differ. what is left of a parallel one is no tangent (w = 0)
		auto tangent = context.cornerTangents[cornerIt];
		if (tangent.w != 0.0F)
		{
			auto normal = glm::make_vec3(vertices[index].normal);
			auto perpendicular = glm::vec3(tangent) - (glm::dot(glm::vec3(tangent), normal) * normal);
			auto length = glm::length(perpendicular);
			tangent = std::isfinite(length) && length > 1e-3F ? glm::vec4(perpendicular / length, tangent.w) : glm::vec4(0.0F);
		}
		auto& vertexCopies = copies[index];
		auto it = std::ranges::find_if(vertexCopies, [&tangent](const auto& copy) { return copy.first == tangent; });
		if (it == vertexCopies.end())
		{
			auto vertex = vertices[index];
			std::copy_n(glm::value_ptr(tangent), 4, vertex.tangent);
			result.push_back(vertex);
			sources.push_back(index);
			it = vertexCopies.insert(vertexCopies.end(), {tangent, static_cast<uint32_t>(result.size() - 1)});
		}
		indices[cornerIt] = it->second;
	}
	vertices = std::move(result);
	return sources;
}

} // namespace gfx::mesh
