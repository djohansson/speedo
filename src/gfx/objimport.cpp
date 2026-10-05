#include "objimport.h"

#include <core/profiling.h>
#include <core/utils.h>

#include <algorithm>
#include <bit>
#include <cctype>
#include <cmath>
#include <format>
#include <optional>
#include <system_error>

#define TINYOBJLOADER_IMPLEMENTATION
#include <tiny_obj_loader.h>

#include <xxhash.h>

namespace gfx::obj
{

using mesh::Mesh;
using mesh::Submesh;

namespace detail
{

// finds the files an mtl file names. those are often written on windows, so they may use backslashes and a different
// case than the files on disk, and some are only found by name somewhere under the obj file's directory.
class TextureResolver
{
public:
	explicit TextureResolver(std::filesystem::path baseDir)
		: myBaseDir(std::move(baseDir))
	{}

	[[nodiscard]] std::optional<std::filesystem::path> Resolve(std::string name)
	{
		std::ranges::replace(name, '\\', '/');
		while (name.starts_with("./"))
			name.erase(0, 2);

		std::filesystem::path path(name);
		if (path.is_absolute())
		{
			if (std::error_code error; std::filesystem::is_regular_file(path, error))
				return path;
			path = path.filename(); // look for it by name below
		}
		else if (std::error_code error; std::filesystem::is_regular_file(myBaseDir / path, error))
		{
			return myBaseDir / path;
		}

		InternalIndex();

		if (auto found = myFiles.find(ToLower(path.generic_string())); found != myFiles.end())
			return found->second;

		if (auto found = myFileNames.find(ToLower(path.filename().string())); found != myFileNames.end())
			return found->second;

		return std::nullopt;
	}

private:
	[[nodiscard]] static std::string ToLower(std::string str)
	{
		std::ranges::transform(str, str.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
		return str;
	}

	// lists the files under the base directory once, by lower case relative path and by lower case file name
	void InternalIndex()
	{
		if (myIndexed)
			return;

		myIndexed = true;

		std::error_code error;
		for (auto it = std::filesystem::recursive_directory_iterator(myBaseDir, error);
			 !error && it != std::filesystem::recursive_directory_iterator();
			 it.increment(error))
		{
			if (!it->is_regular_file(error))
				continue;

			const auto& file = it->path();
			myFiles.emplace(ToLower(file.lexically_relative(myBaseDir).generic_string()), file);
			myFileNames.emplace(ToLower(file.filename().string()), file); // keeps the first one found
		}
	}

	std::filesystem::path myBaseDir;
	bool myIndexed = false;
	core::UnorderedMap<std::string, std::filesystem::path> myFiles;
	core::UnorderedMap<std::string, std::filesystem::path> myFileNames;
};

// a vertex is welded with the others that have the same key. normal is -1 for a vertex whose normal is generated, which
// is then part of the key (as bits) instead.
struct VertexKey
{
	int32_t position = 0;
	int32_t normal = 0;
	int32_t texCoord = 0;
	int32_t material = 0;
	std::array<uint32_t, 3> generatedNormal{};

	[[nodiscard]] bool operator==(const VertexKey&) const = default;
};

struct VertexKeyHash
{
	using is_avalanching = void; //NOLINT(readability-identifier-naming)

	[[nodiscard]] uint64_t operator()(const VertexKey& key) const noexcept { return XXH3_64bits(&key, sizeof(key)); }
};

static_assert(std::has_unique_object_representations_v<VertexKey>, "VertexKey is hashed as bytes");

// a face around a position whose normal is generated
struct AdjacentFace
{
	std::array<float, 3> normal{}; // not normalized: its length is twice the face's area
	uint32_t smoothingGroup = 0;
};

// faces in smoothing group 0 (off, or not given) are smoothed with the faces around them that are within this angle,
// so that hard edges stay hard and curved surfaces without normals come out smooth
constexpr double kCreaseAngleCos = 0.5; // 60 degrees

// a triangle of a face in a shape: corners first, second and second + 1 of the shape's indices. part numbers the runs
// of faces with the same material in the shapes, which often are separate objects, merged into one group on export.
struct TriangleRef
{
	uint32_t shape = 0;
	uint32_t face = 0;
	uint32_t first = 0;
	uint32_t second = 0;
	uint32_t part = 0;
};

using Vec3 = std::array<double, 3>;

[[nodiscard]] Vec3 Sub(const Vec3& a, const Vec3& b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
[[nodiscard]] Vec3 Cross(const Vec3& a, const Vec3& b)
{
	return {(a[1] * b[2]) - (a[2] * b[1]), (a[2] * b[0]) - (a[0] * b[2]), (a[0] * b[1]) - (a[1] * b[0])};
}
[[nodiscard]] double Dot(const Vec3& a, const Vec3& b) { return (a[0] * b[0]) + (a[1] * b[1]) + (a[2] * b[2]); }
[[nodiscard]] double Length(const Vec3& a) { return std::sqrt(Dot(a, a)); }
[[nodiscard]] bool IsFinite(const Vec3& a) { return std::isfinite(a[0]) && std::isfinite(a[1]) && std::isfinite(a[2]); }

} // namespace detail

std::expected<Mesh, std::string> Import(const std::filesystem::path& path, const std::function<bool()>& cancelled)
{
	using namespace detail;

	ZoneScopedN("obj::Import");

	auto isCancelled = [&cancelled] { return cancelled && cancelled(); };

	tinyobj::ObjReaderConfig config;
	config.triangulate = true;
	config.vertex_color = true;
	config.mtl_search_path = path.parent_path().string();

	tinyobj::ObjReader reader;
	{
		ZoneScopedN("obj::Import::parse");

		if (!reader.ParseFromFile(path.string(), config))
			return std::unexpected(std::format("failed to parse {}: {}", path.string(), reader.Error()));
	}

	if (isCancelled())
		return std::unexpected("cancelled");

	const auto& attrib = reader.GetAttrib();
	const auto& shapes = reader.GetShapes();
	const auto& objMaterials = reader.GetMaterials();

	Mesh mesh;
	auto& stats = mesh.stats;

	for (std::string_view warnings = reader.Warning(); !warnings.empty();)
	{
		auto end = warnings.find('\n');
		if (auto line = warnings.substr(0, end); !line.empty())
			stats.warnings.emplace_back(line);
		warnings = end == std::string_view::npos ? std::string_view{} : warnings.substr(end + 1);
	}

	TextureResolver textures(path.parent_path());
	auto resolveTexture = [&textures, &stats](const std::string& name, const std::string& material) -> std::filesystem::path
	{
		if (name.empty())
			return {};

		if (auto resolved = textures.Resolve(name))
			return *resolved;

		stats.missingTextures++;
		stats.warnings.emplace_back(std::format("material {}: texture not found: {}", material, name));

		return {};
	};

	mesh.materials.reserve(objMaterials.size());
	for (const auto& objMaterial : objMaterials)
	{
		auto& material = mesh.materials.emplace_back();
		material.name = objMaterial.name;
		std::ranges::copy(objMaterial.diffuse, material.diffuse.begin());
		material.dissolve = objMaterial.dissolve;
		material.diffuseTexture.path = resolveTexture(objMaterial.diffuse_texname, objMaterial.name).string();
		material.alphaTexture.path = resolveTexture(objMaterial.alpha_texname, objMaterial.name).string();
		material.normalTexture.path = resolveTexture(objMaterial.normal_texname, objMaterial.name).string();
		std::ranges::copy(objMaterial.emission, material.emissive.begin());
		material.emissiveTexture.path = resolveTexture(objMaterial.emissive_texname, objMaterial.name).string();
		material.bumpTexture.path = resolveTexture(objMaterial.bump_texname, objMaterial.name).string();
		material.bumpScale = objMaterial.bump_texopt.bump_multiplier;
	}

	const auto positionCount = attrib.vertices.size() / 3;
	const auto normalCount = attrib.normals.size() / 3;
	const auto texCoordCount = attrib.texcoords.size() / 2;

	auto position = [&attrib](int32_t index)
	{
		return Vec3{attrib.vertices[3UL * index], attrib.vertices[(3UL * index) + 1], attrib.vertices[(3UL * index) + 2]};
	};
	auto normal = [&attrib](int32_t index)
	{
		return Vec3{attrib.normals[3UL * index], attrib.normals[(3UL * index) + 1], attrib.normals[(3UL * index) + 2]};
	};

	// normals that can't be used (zero length or non-finite) are treated as missing, and generated
	std::vector<bool> normalValid(normalCount);
	for (size_t normalIt = 0; normalIt < normalCount; normalIt++)
	{
		auto n = normal(static_cast<int32_t>(normalIt));
		normalValid[normalIt] = IsFinite(n) && Length(n) > 0.0;
	}

	auto hasNormal = [&normalValid, normalCount](int32_t index)
	{
		return index >= 0 && static_cast<size_t>(index) < normalCount && normalValid[index];
	};

	mesh.hasNormals = normalCount > 0;
	mesh.hasTexCoords = texCoordCount > 0;
	// tinyobj fills in white for vertices without a color
	mesh.hasColors = std::ranges::any_of(attrib.colors, [](tinyobj::real_t c) { return c != 1.0F; });

	auto materialOf = [&mesh](int32_t id) { return id >= 0 && static_cast<size_t>(id) < mesh.materials.size() ? id : -1; };

	auto faceNormalOf = [&position](const std::array<tinyobj::index_t, 3>& corners)
	{
		return Cross(
			Sub(position(corners[1].vertex_index), position(corners[0].vertex_index)),
			Sub(position(corners[2].vertex_index), position(corners[0].vertex_index)));
	};

	// parts are flipped (once we know which) by swapping the last two corners of their triangles
	std::vector<bool> partFlipped;
	auto cornersOf = [&shapes, &partFlipped](const TriangleRef& triangle)
	{
		const auto& indices = shapes[triangle.shape].mesh.indices;
		if (triangle.part < partFlipped.size() && partFlipped[triangle.part])
			return std::array{indices[triangle.first], indices[triangle.second + 1], indices[triangle.second]};
		return std::array{indices[triangle.first], indices[triangle.second], indices[triangle.second + 1]};
	};

	auto smoothingGroupOf = [&shapes](const TriangleRef& triangle)
	{
		const auto& ids = shapes[triangle.shape].mesh.smoothing_group_ids;
		return triangle.face < ids.size() ? ids[triangle.face] : 0U;
	};

	// the triangles, by material (-1 last), and how many faces are around each position that has corners without normals
	std::vector<std::vector<TriangleRef>> trianglesByMaterial(mesh.materials.size() + 1);
	std::vector<uint32_t> adjacentFaceOffsets; // per position, then turned into offsets into adjacentFaces
	std::vector<double> partAgreeingArea;
	std::vector<double> partNormalArea;
	{
		ZoneScopedN("obj::Import::triangles");

		for (uint32_t shapeIt = 0; shapeIt < shapes.size(); shapeIt++)
		{
			const auto& objMesh = shapes[shapeIt].mesh;
			uint32_t first = 0;
			std::optional<int32_t> partMaterial;
			for (uint32_t faceIt = 0; faceIt < objMesh.num_face_vertices.size(); faceIt++)
			{
				auto cornerCount = static_cast<uint32_t>(objMesh.num_face_vertices[faceIt]);
				auto material = materialOf(faceIt < objMesh.material_ids.size() ? objMesh.material_ids[faceIt] : -1);

				if (partMaterial != material)
				{
					partMaterial = material;
					partAgreeingArea.push_back(0.0);
					partNormalArea.push_back(0.0);
				}
				auto part = static_cast<uint32_t>(partNormalArea.size() - 1);

				// faces are triangulated already, fan out anything that isn't
				for (uint32_t second = first + 1; second + 1 < first + cornerCount; second++)
				{
					TriangleRef triangle{.shape = shapeIt, .face = faceIt, .first = first, .second = second, .part = part};
					auto corners = cornersOf(triangle);

					if (std::ranges::any_of(corners, [positionCount](const auto& corner)
						{ return corner.vertex_index < 0 || static_cast<size_t>(corner.vertex_index) >= positionCount; }))
					{
						stats.droppedTriangles++;
						continue;
					}

					stats.triangleCount++;
					trianglesByMaterial[material >= 0 ? material : mesh.materials.size()].push_back(triangle);

					auto faceNormal = faceNormalOf(corners);
					auto area = Length(faceNormal);
					if (!std::isfinite(area) || area == 0.0)
					{
						stats.degenerateTriangles++;
						continue;
					}

					if (std::ranges::all_of(corners, [&hasNormal](const auto& corner) { return hasNormal(corner.normal_index); }))
					{
						Vec3 vertexNormals{};
						for (const auto& corner : corners)
						{
							auto n = normal(corner.normal_index);
							auto length = Length(n);
							for (size_t i = 0; i < 3; i++)
								vertexNormals[i] += n[i] / length;
						}
						partNormalArea[part] += area;
						if (Dot(faceNormal, vertexNormals) > 0.0)
							partAgreeingArea[part] += area;
						continue;
					}

					if (adjacentFaceOffsets.empty())
						adjacentFaceOffsets.resize(positionCount + 1);

					for (const auto& corner : corners)
						adjacentFaceOffsets[corner.vertex_index]++;
				}

				first += cornerCount;
			}
		}
	}

	double agreeingArea = 0.0;
	double normalArea = 0.0;
	partFlipped.resize(partNormalArea.size());
	for (size_t partIt = 0; partIt < partNormalArea.size(); partIt++)
	{
		partFlipped[partIt] = partAgreeingArea[partIt] < 0.5 * partNormalArea[partIt];
		if (partFlipped[partIt])
			stats.flippedParts++;

		normalArea += partNormalArea[partIt];
		agreeingArea += partFlipped[partIt] ? partNormalArea[partIt] - partAgreeingArea[partIt] : partAgreeingArea[partIt];
	}
	if (normalArea > 0.0)
		stats.windingAgreement = agreeingArea / normalArea;

	if (isCancelled())
		return std::unexpected("cancelled");

	// the faces around the positions of corners without normals, which their normals are generated from
	std::vector<AdjacentFace> adjacentFaces;
	if (!adjacentFaceOffsets.empty())
	{
		ZoneScopedN("obj::Import::adjacency");

		uint32_t offset = 0;
		for (auto& count : adjacentFaceOffsets)
			offset += std::exchange(count, offset);

		adjacentFaces.resize(offset);
		auto next = adjacentFaceOffsets;
		for (const auto& triangles : trianglesByMaterial)
		{
			for (const auto& triangle : triangles)
			{
				auto corners = cornersOf(triangle);
				if (std::ranges::all_of(corners, [&hasNormal](const auto& corner) { return hasNormal(corner.normal_index); }))
					continue;

				auto faceNormal = faceNormalOf(corners);
				auto area = Length(faceNormal);
				if (!std::isfinite(area) || area == 0.0)
					continue;

				AdjacentFace face{
					.normal = {static_cast<float>(faceNormal[0]), static_cast<float>(faceNormal[1]), static_cast<float>(faceNormal[2])},
					.smoothingGroup = smoothingGroupOf(triangle)};
				for (const auto& corner : corners)
					adjacentFaces[next[corner.vertex_index]++] = face;
			}
		}
	}

	// the area weighted sum of the face normals around a corner's position that are in its smoothing group, or for
	// group 0 within the crease angle of its own face
	auto generateNormal = [&](int32_t positionIndex, const Vec3& faceNormal, uint32_t smoothingGroup)
	{
		Vec3 sum{};
		auto faceLength = Length(faceNormal);
		for (auto faceIt = adjacentFaceOffsets[positionIndex]; faceIt < adjacentFaceOffsets[positionIndex + 1]; faceIt++)
		{
			const auto& face = adjacentFaces[faceIt];
			if (face.smoothingGroup != smoothingGroup)
				continue;

			Vec3 n{face.normal[0], face.normal[1], face.normal[2]};
			if (smoothingGroup == 0 && faceLength > 0.0 && Dot(n, faceNormal) < kCreaseAngleCos * Length(n) * faceLength)
				continue;

			for (size_t i = 0; i < 3; i++)
				sum[i] += n[i];
		}
		return sum;
	};

	{
		ZoneScopedN("obj::Import::vertices");

		core::UnorderedMap<VertexKey, uint32_t, VertexKeyHash> vertexIndices;
		vertexIndices.reserve(stats.triangleCount);
		mesh.vertices.reserve(stats.triangleCount); // roughly, for a closed mesh
		mesh.indices.reserve(stats.triangleCount * 3);

		bool firstVertex = true;

		auto addVertex = [&](const tinyobj::index_t& corner, const Vec3& faceNormal, int32_t material, uint32_t smoothingGroup) -> uint32_t
		{
			bool fileNormal = hasNormal(corner.normal_index);
			bool fileTexCoord = corner.texcoord_index >= 0 && static_cast<size_t>(corner.texcoord_index) < texCoordCount;

			Vec3 n;
			if (fileNormal)
			{
				n = normal(corner.normal_index);
			}
			else
			{
				n = generateNormal(corner.vertex_index, faceNormal, smoothingGroup);
				if (!(Length(n) > 0.0)) // only around degenerate faces
					n = {0.0, 1.0, 0.0};
			}
			auto length = Length(n);
			std::array<float, 3> unitNormal{
				static_cast<float>(n[0] / length), static_cast<float>(n[1] / length), static_cast<float>(n[2] / length)};

			VertexKey key{
				.position = corner.vertex_index,
				.normal = fileNormal ? corner.normal_index : -1,
				.texCoord = fileTexCoord ? corner.texcoord_index : -1,
				.material = material,
				.generatedNormal = fileNormal ? std::array<uint32_t, 3>{} : std::bit_cast<std::array<uint32_t, 3>>(unitNormal)};

			auto [it, inserted] = vertexIndices.try_emplace(key, static_cast<uint32_t>(mesh.vertices.size()));
			if (!inserted)
				return it->second;

			auto& vertex = mesh.vertices.emplace_back();

			auto p = position(corner.vertex_index);
			if (!IsFinite(p))
			{
				stats.nonFiniteValues++;
				p = {};
			}
			std::ranges::transform(p, vertex.position, [](double v) { return static_cast<float>(v); });

			std::ranges::copy(unitNormal, vertex.normal);
			if (!fileNormal)
			{
				stats.generatedNormals++;
				if (corner.normal_index >= 0)
					stats.repairedNormals++;
			}

			if (fileTexCoord)
			{
				auto u = attrib.texcoords[2UL * corner.texcoord_index];
				auto v = attrib.texcoords[(2UL * corner.texcoord_index) + 1];
				if (!std::isfinite(u) || !std::isfinite(v))
				{
					stats.nonFiniteValues++;
					u = v = 0.0F;
				}
				vertex.texCoord01[0] = u;
				vertex.texCoord01[1] = 1.0F - v;
			}

			std::array<float, 4> color{1.0F, 1.0F, 1.0F, 1.0F};
			if (mesh.hasColors && (static_cast<size_t>(corner.vertex_index) * 3) + 2 < attrib.colors.size())
				for (size_t i = 0; i < 3; i++)
					color[i] = attrib.colors[(3UL * corner.vertex_index) + i];
			if (material >= 0)
			{
				const auto& m = mesh.materials[material];
				for (size_t i = 0; i < 3; i++)
					color[i] *= m.diffuse[i];
				color[3] = m.dissolve;
			}
			std::ranges::copy(color, vertex.color);

			if (firstVertex)
			{
				mesh.bounds.SetMin(Bounds3f::VectorType(vertex.position[0], vertex.position[1], vertex.position[2]));
				mesh.bounds.SetMax(mesh.bounds.GetMin());
				firstVertex = false;
			}
			else
			{
				mesh.bounds.Merge(std::to_array(vertex.position));
			}

			return it->second;
		};

		for (size_t bucketIt = 0; bucketIt < trianglesByMaterial.size(); bucketIt++)
		{
			auto& triangles = trianglesByMaterial[bucketIt];
			if (triangles.empty())
				continue;

			auto material = bucketIt < mesh.materials.size() ? static_cast<int32_t>(bucketIt) : -1;
			auto& submesh = mesh.submeshes.emplace_back(Submesh{
				.firstIndex = static_cast<uint32_t>(mesh.indices.size()), .indexCount = 0, .material = material});

			for (const auto& triangle : triangles)
			{
				auto corners = cornersOf(triangle);
				auto faceNormal = faceNormalOf(corners);
				auto smoothingGroup = smoothingGroupOf(triangle);

				for (const auto& corner : corners)
					mesh.indices.push_back(addVertex(corner, faceNormal, material, smoothingGroup));
			}

			submesh.indexCount = static_cast<uint32_t>(mesh.indices.size()) - submesh.firstIndex;

			std::vector<TriangleRef>().swap(triangles);

			if (isCancelled())
				return std::unexpected("cancelled");
		}
	}

	return mesh;
}

std::vector<std::filesystem::path> MaterialFiles(const std::filesystem::path& path)
{
	std::vector<std::filesystem::path> files;

	std::error_code error;
	for (auto it = std::filesystem::directory_iterator(path.parent_path(), error);
		 !error && it != std::filesystem::directory_iterator();
		 it.increment(error))
	{
		auto extension = it->path().extension().string();
		std::ranges::transform(extension, extension.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
		if (extension == ".mtl" && it->is_regular_file(error))
			files.push_back(it->path());
	}

	std::ranges::sort(files);

	return files;
}

} // namespace gfx::obj
