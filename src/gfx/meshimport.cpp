#include <gfx/gltfimport.h>
#include <gfx/meshimport.h>
#include <gfx/objimport.h>

#include <core/profiling.h>

#include <algorithm>
#include <cctype>
#include <format>
#include <limits>
#include <span>
#include <string>
#include <vector>

#include <meshoptimizer.h>

namespace gfx::mesh
{

namespace detail
{

enum class Format : uint8_t
{
	kUnknown,
	kObj,
	kGltf,
};

[[nodiscard]] Format FormatOf(const std::filesystem::path& path)
{
	auto extension = path.extension().string();
	std::ranges::transform(extension, extension.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
	if (extension == ".obj")
		return Format::kObj;
	if (extension == ".gltf" || extension == ".glb")
		return Format::kGltf;
	return Format::kUnknown;
}

// the bounds of submesh's vertices at each of its instances, in world space
[[nodiscard]] Bounds3f InstanceBounds(const Mesh& mesh, const Submesh& submesh)
{
	auto indices = std::span(mesh.indices).subspan(submesh.firstIndex, submesh.indexCount);
	if (indices.empty())
		return {};

	Bounds3f local;
	local.SetMin(Bounds3f::VectorType(std::numeric_limits<float>::max()));
	local.SetMax(Bounds3f::VectorType(std::numeric_limits<float>::lowest()));
	for (auto index : indices)
		local.Merge(std::to_array(mesh.vertices[index].position));

	Bounds3f world;
	bool first = true;
	for (uint32_t instanceIt = submesh.firstInstance; instanceIt < submesh.firstInstance + submesh.instanceCount; instanceIt++)
	{
		const auto& transform = mesh.instances[instanceIt];
		for (const auto& corner : local.Corners())
		{
			Bounds3f::VectorType point;
			for (glm::length_t axis = 0; axis < 3; axis++)
				point[axis] = (transform[axis] * corner.x) + (transform[4 + axis] * corner.y) + (transform[8 + axis] * corner.z) +
							  transform[12 + axis];
			if (first)
			{
				world.SetMin(point);
				world.SetMax(point);
				first = false;
			}
			else
				world.Merge(point);
		}
	}
	return world;
}

} // namespace detail

bool IsModelFile(const std::filesystem::path& path)
{
	return detail::FormatOf(path) != detail::Format::kUnknown;
}

std::expected<Mesh, std::string> Import(
	const std::filesystem::path& path, const ImportOptions& options, const std::function<bool()>& cancelled)
{
	switch (detail::FormatOf(path))
	{
	case detail::Format::kObj: return obj::Import(path, cancelled);
	case detail::Format::kGltf: return gltf::Import(path, options, cancelled);
	default: return std::unexpected(std::format("{}: not a model file (.obj, .gltf or .glb)", path.string()));
	}
}

std::optional<std::string> Unsupported(const std::filesystem::path& path)
{
	if (detail::FormatOf(path) == detail::Format::kGltf)
		if (auto extension = gltf::UnsupportedRequiredExtension(path))
			return std::format("it requires {}, which isn't supported", *extension);
	return std::nullopt;
}

std::vector<std::filesystem::path> Dependencies(const std::filesystem::path& path)
{
	switch (detail::FormatOf(path))
	{
	case detail::Format::kObj: return obj::MaterialFiles(path);
	case detail::Format::kGltf: return gltf::BufferFiles(path);
	default: return {};
	}
}

void Chunk(Mesh& mesh, uint32_t maxTriangles)
{
	ZoneScopedN("gfx::mesh::Chunk");

	maxTriangles = std::max(maxTriangles, 1U);

	// instances that follow nodes animations move (or hide)
	std::vector<bool> linked(mesh.instances.size(), false);
	for (const auto& link : mesh.animation.instanceLinks)
		if (link.instance < linked.size())
			linked[link.instance] = true;

	std::vector<Submesh> chunks;
	chunks.reserve(mesh.submeshes.size());
	std::vector<uint32_t> indices;
	std::vector<float> centroids;
	std::vector<uint32_t> remap;
	std::vector<uint32_t> order;
	for (auto submesh : mesh.submeshes)
	{
		submesh.moves = submesh.skin >= 0 || submesh.morphTargetCount > 0 ||
						std::any_of(
							linked.begin() + submesh.firstInstance, linked.begin() + submesh.firstInstance + submesh.instanceCount,
							[](bool link) { return link; });

		auto triangleCount = submesh.indexCount / 3;
		if (submesh.moves || submesh.topology != rhi::PrimitiveTopology::kTriangleList || triangleCount <= maxTriangles)
		{
			submesh.bounds = detail::InstanceBounds(mesh, submesh);
			chunks.push_back(submesh);
			continue;
		}

		// the triangles by place: their centroids along a space filling curve
		auto submeshIndices = std::span(mesh.indices).subspan(submesh.firstIndex, submesh.indexCount);
		centroids.resize(static_cast<size_t>(triangleCount) * 3);
		for (uint32_t triangleIt = 0; triangleIt < triangleCount; triangleIt++)
			for (size_t axis = 0; axis < 3; axis++)
				centroids[(triangleIt * 3) + axis] = (mesh.vertices[submeshIndices[(triangleIt * 3) + 0]].position[axis] +
													  mesh.vertices[submeshIndices[(triangleIt * 3) + 1]].position[axis] +
													  mesh.vertices[submeshIndices[(triangleIt * 3) + 2]].position[axis]) /
													 3.0F;
		remap.resize(triangleCount);
		meshopt_spatialSortRemap(remap.data(), centroids.data(), triangleCount, sizeof(float) * 3);
		order.resize(triangleCount);
		for (uint32_t triangleIt = 0; triangleIt < triangleCount; triangleIt++)
			order[remap[triangleIt]] = triangleIt;

		// runs of the curve, as even as they can be, each in the file's order
		auto chunkCount = (triangleCount + maxTriangles - 1) / maxTriangles;
		indices.clear();
		indices.reserve(submesh.indexCount);
		for (uint32_t chunkIt = 0; chunkIt < chunkCount; chunkIt++)
		{
			auto begin = static_cast<size_t>(triangleCount) * chunkIt / chunkCount;
			auto end = static_cast<size_t>(triangleCount) * (chunkIt + 1) / chunkCount;
			std::sort(order.begin() + static_cast<ptrdiff_t>(begin), order.begin() + static_cast<ptrdiff_t>(end));

			auto chunk = submesh;
			chunk.firstIndex = submesh.firstIndex + static_cast<uint32_t>(indices.size());
			chunk.indexCount = static_cast<uint32_t>((end - begin) * 3);
			for (auto triangleIt = begin; triangleIt < end; triangleIt++)
				for (size_t corner = 0; corner < 3; corner++)
					indices.push_back(submeshIndices[(order[triangleIt] * 3) + corner]);
			chunks.push_back(chunk);
		}
		std::ranges::copy(indices, submeshIndices.begin());
		for (auto chunkIt = chunks.size() - chunkCount; chunkIt < chunks.size(); chunkIt++)
			chunks[chunkIt].bounds = detail::InstanceBounds(mesh, chunks[chunkIt]);
	}
	mesh.submeshes = std::move(chunks);
}

std::vector<std::filesystem::path> ImageFiles(const std::filesystem::path& path)
{
	switch (detail::FormatOf(path))
	{
	case detail::Format::kObj: return obj::TextureFiles(path);
	case detail::Format::kGltf: return gltf::ImageFiles(path);
	default: return {};
	}
}

} // namespace gfx::mesh
