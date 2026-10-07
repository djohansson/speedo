// imports models and images the way the client does (gfx::mesh::Import, gfx::image::Import) and checks the results.
// usage: assettest [--models-only | --images-only] <file, directory or zip archive>...
// directories (and zip archives, extracted to a temporary directory) are searched recursively for models (.obj, .gltf, .glb) and images. the textures that models' materials name are
// checked too. prints one line per asset (PASS, WARN or FAIL, with details) and a summary, and exits with 1 if any
// asset failed.

#include <gfx/environment.h>
#include <gfx/imageimport.h>
#include <gfx/gltfimport.h>
#include <gfx/meshimport.h>
#include <gfx/ziparchive.h>

#define BCDEC_IMPLEMENTATION
#define BCDEC_STATIC
#include <bcdec.h>

#include <glm/glm.hpp>
#include <glm/gtc/packing.hpp>

#include <core/utils.h>

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <format>
#include <iterator>
#include <fstream>
#include <numbers>
#include <optional>
#include <print>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

namespace
{

enum class Result : uint8_t
{
	kPass,
	kWarn,
	kFail,
};

struct Report
{
	Result result = Result::kPass;
	std::vector<std::string> notes;

	template <typename... Args>
	void Warn(std::format_string<Args...> fmt, Args&&... args)
	{
		result = std::max(result, Result::kWarn);
		notes.push_back("warn: " + std::format(fmt, std::forward<Args>(args)...));
	}

	template <typename... Args>
	void Fail(std::format_string<Args...> fmt, Args&&... args)
	{
		result = Result::kFail;
		notes.push_back("FAIL: " + std::format(fmt, std::forward<Args>(args)...));
	}

	template <typename... Args>
	void Info(std::format_string<Args...> fmt, Args&&... args)
	{
		notes.push_back(std::format(fmt, std::forward<Args>(args)...));
	}
};

constexpr std::string_view ToString(Result result)
{
	switch (result)
	{
	case Result::kPass: return "PASS";
	case Result::kWarn: return "WARN";
	case Result::kFail: return "FAIL";
	}
	return "?";
}

uint32_t IndicesPerPrimitive(rhi::PrimitiveTopology topology)
{
	switch (topology)
	{
	case rhi::PrimitiveTopology::kTriangleList: return 3;
	case rhi::PrimitiveTopology::kLineList: return 2;
	case rhi::PrimitiveTopology::kPointList: return 1;
	}
	return 3;
}

std::string Lower(std::string str)
{
	std::ranges::transform(str, str.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
	return str;
}

bool IsModel(const std::filesystem::path& path) { return gfx::mesh::IsModelFile(path); }

bool IsImage(const std::filesystem::path& path)
{
	static constexpr std::array<std::string_view, 13> kExtensions{
		".png", ".jpg", ".jpeg", ".tga", ".bmp", ".psd", ".gif", ".pic", ".pnm", ".ppm", ".pgm", ".webp", ".ktx2"};
	return std::ranges::contains(kExtensions, Lower(path.extension().string()));
}

// environment panoramas (see gfx::environment), which the client loads as such rather than as images
bool IsEnvironment(const std::filesystem::path& path) { return Lower(path.extension().string()) == ".hdr"; }

using Vec3 = std::array<double, 3>;

Vec3 ToVec3(const float (&v)[3]) { return {v[0], v[1], v[2]}; } //NOLINT(modernize-avoid-c-arrays)

// an image (a file, or an image the gltf file at the path embeds, see TextureRef::embeddedImage), with the usage it is
// checked for, and its bump scale for kBump
using ImageCheck = std::tuple<std::filesystem::path, std::optional<uint32_t>, gfx::image::Usage, float>;

// the images to check, each (with its usage and bump scale) once
using ImageChecks = core::UnorderedMap<std::string, ImageCheck>;

void Add(ImageChecks& checks, std::filesystem::path path, std::optional<uint32_t> embeddedImage, gfx::image::Usage usage, float bumpScale)
{
	auto key = std::format("{}|{}|{}|{}", path.string(), embeddedImage.value_or(~0U), std::to_underlying(usage), bumpScale);
	checks.try_emplace(std::move(key), std::move(path), embeddedImage, usage, bumpScale);
}

void Add(ImageChecks& checks, const gfx::TextureRef& texture, gfx::image::Usage usage, float bumpScale)
{
	Add(checks, std::filesystem::weakly_canonical(texture.path), texture.embeddedImage, usage, bumpScale);
}

// counts of each Result
using ResultCounts = std::array<size_t, 3>;

[[nodiscard]] size_t& Count(ResultCounts& counts, Result result) { return counts[static_cast<size_t>(result)]; }

// what an import produced, to compare the encodings of a model with (see CheckEncodings)
struct ModelSummary
{
	size_t triangles = 0;
	size_t vertices = 0;
	size_t materials = 0;
	size_t submeshes = 0;
	size_t textures = 0; // texture slots the materials use
	Vec3 min{};
	Vec3 max{};
};

Report CheckModel(const std::filesystem::path& path, ImageChecks& texturesOut, std::optional<ModelSummary>& summaryOut)
{
	Report report;

	// embedded gltf images are checked like the others, read from the file (see TextureRef::embeddedImage)
	auto mesh = gfx::mesh::Import(
		path, {});
	if (!mesh)
	{
		report.Fail("{}", mesh.error());
		return report;
	}

	const auto& stats = mesh->stats;

	// the file's other scenes import too (the checks below are of the default one)
	if (mesh->scenes.size() > 1)
	{
		report.Info("{} scenes, scene {} ({}) is checked", mesh->scenes.size(), mesh->scene, mesh->scenes[mesh->scene]);
		for (size_t sceneIt = 0; sceneIt < mesh->scenes.size(); sceneIt++)
		{
			if (sceneIt == mesh->scene)
				continue;
			auto other = gfx::mesh::Import(
				path,
				{.scene = sceneIt});
			if (!other)
				report.Fail("scene {} ({}): {}", sceneIt, mesh->scenes[sceneIt], other.error());
			else if (other->scene != sceneIt)
				report.Fail("scene {} ({}): scene {} was imported instead", sceneIt, mesh->scenes[sceneIt], other->scene);
			else if (other->indices.empty())
				report.Warn("scene {} ({}) has no primitives", sceneIt, mesh->scenes[sceneIt]);
			else
				report.Info(
					"scene {} ({}): {} triangles, {} vertices", sceneIt, mesh->scenes[sceneIt], other->stats.triangleCount,
					other->vertices.size());
		}
	}

	{
		auto& summary = summaryOut.emplace();
		summary.triangles = stats.triangleCount;
		summary.vertices = mesh->vertices.size();
		summary.materials = mesh->materials.size();
		summary.submeshes = mesh->submeshes.size();
		for (const auto& material : mesh->materials)
			for (const auto* texture : {&material.diffuseTexture, &material.alphaTexture, &material.normalTexture, &material.bumpTexture})
				summary.textures += texture->empty() ? 0 : 1;
		auto min = mesh->bounds.GetMin();
		auto max = mesh->bounds.GetMax();
		summary.min = {min.x, min.y, min.z};
		summary.max = {max.x, max.y, max.z};
	}

	report.Info(
		"{} triangles{}, {} vertices, {} materials, {} submeshes, normals: {}, tangents: {}, texcoords: {}, colors: {}",
		stats.triangleCount,
		stats.lineCount + stats.pointCount > 0 ? std::format(", {} lines, {} points", stats.lineCount, stats.pointCount) : "",
		mesh->vertices.size(), mesh->materials.size(), mesh->submeshes.size(),
		mesh->hasNormals ? "file" : "generated", mesh->hasTangents ? "file" : stats.generatedTangents > 0 ? "generated" : "derived", mesh->hasTexCoords ? "yes" : "no", mesh->hasColors ? "yes" : "no");

	if (mesh->instances.size() > 1)
		report.Info("{} instances (EXT_mesh_gpu_instancing, or of moving nodes)", mesh->instances.size() - 1);
	if (const auto& animation = mesh->animation; !animation.Empty())
		report.Info(
			"{} skins ({} joints), {} animations, {} instances follow nodes", animation.skins.size(), animation.jointCount,
			animation.animations.size(), animation.instanceLinks.size());
	// every animation evaluates to finite transforms, and crossfading from the rest pose starts at it and ends at the
	// animation's own pose
	if (const auto& animation = mesh->animation; !animation.Empty())
	{
		auto maxDifference = [](const std::vector<gfx::SceneMatrix>& a, const std::vector<gfx::SceneMatrix>& b)
		{
			double difference = 0.0;
			for (size_t nodeIt = 0; nodeIt < std::min(a.size(), b.size()); nodeIt++)
				for (size_t i = 0; i < 16; i++)
					difference = std::max(difference, static_cast<double>(std::abs(a[nodeIt][i] - b[nodeIt][i])));
			return difference;
		};
		auto rest = gfx::EvaluateNodes(animation, animation.animations.size(), 0.0F);
		for (size_t animationIt = 0; animationIt < animation.animations.size(); animationIt++)
		{
			const auto& clip = animation.animations[animationIt];
			for (auto fraction : {0.0F, 0.37F, 0.81F})
			{
				auto time = fraction * clip.duration;
				auto worlds = gfx::EvaluateNodes(animation, animationIt, time);
				if (std::ranges::any_of(worlds, [](const auto& m) { return std::ranges::any_of(m, [](float v) { return !std::isfinite(v); }); }))
				{
					report.Fail("animation {} ({}) has non-finite transforms at {:.2f}s", animationIt, clip.name, time);
					break;
				}
				gfx::ScenePose pose{.animation = animationIt, .time = time};
				if (auto d = maxDifference(gfx::EvaluateNodes(animation, pose, {}, 1.0F), worlds); d > 1e-4)
					report.Fail("animation {} ({}): a finished crossfade differs from it by {:.3g}", animationIt, clip.name, d);
				if (auto d = maxDifference(gfx::EvaluateNodes(animation, pose, {}, 0.0F), rest); d > 1e-4)
					report.Fail("animation {} ({}): a crossfade's start differs from the rest pose by {:.3g}", animationIt, clip.name, d);

				if (!animation.pointerTargets.empty())
				{
					auto values = gfx::EvaluatePointers(animation, pose, {}, 1.0F);
					if (values.size() != animation.pointerDefaults.size() || std::ranges::any_of(values, [](float v) { return !std::isfinite(v); }))
						report.Fail("animation {} ({}) has non-finite or missing pointer values at {:.2f}s", animationIt, clip.name, time);
					auto start = gfx::EvaluatePointers(animation, pose, {}, 0.0F);
					if (!std::ranges::equal(start, animation.pointerDefaults, [](float a, float b) { return std::abs(a - b) <= 1e-5F; }))
						report.Fail("animation {} ({}): a crossfade's start differs from the pointer values at rest", animationIt, clip.name);
				}

				if (animation.morphs.empty())
					continue;
				auto weights = gfx::EvaluateWeights(animation, mesh->morphWeights, pose, {}, 1.0F);
				if (weights.size() != mesh->morphWeights.size() || std::ranges::any_of(weights, [](float w) { return !std::isfinite(w); }))
					report.Fail("animation {} ({}) has {} non-finite or missing morph weights at {:.2f}s", animationIt, clip.name, weights.size(), time);
				auto start = gfx::EvaluateWeights(animation, mesh->morphWeights, pose, {}, 0.0F);
				if (!std::ranges::equal(start, mesh->morphWeights, [](float a, float b) { return std::abs(a - b) <= 1e-5F; }))
					report.Fail("animation {} ({}): a crossfade's start differs from the default morph weights", animationIt, clip.name);
			}
		}
	}

	// each submesh with morph targets reads a row of deltas per vertex and its weights
	for (const auto& submesh : mesh->submeshes)
	{
		if (submesh.morphTargetCount == 0 || submesh.firstIndex + submesh.indexCount > mesh->indices.size())
			continue;
		if (submesh.morphWeightBase + submesh.morphTargetCount > mesh->morphWeights.size())
			report.Fail("submesh morph weights {}+{} out of range ({})", submesh.morphWeightBase, submesh.morphTargetCount, mesh->morphWeights.size());
		for (auto index : std::span(mesh->indices).subspan(submesh.firstIndex, submesh.indexCount))
			if (index < submesh.morphFirstVertex ||
				submesh.morphDeltaBase + (static_cast<size_t>(index - submesh.morphFirstVertex) + 1) * submesh.morphTargetCount > mesh->morphDeltas.size())
			{
				report.Fail("submesh morph deltas out of range for vertex {}", index);
				break;
			}
	}

	if (!mesh->skinVertices.empty() && mesh->skinVertices.size() != mesh->vertices.size())
		report.Fail("{} skin vertices for {} vertices", mesh->skinVertices.size(), mesh->vertices.size());

	if (mesh->indices.empty())
	{
		report.Fail("no primitives");
		return report;
	}

	if (auto it = std::ranges::find_if(mesh->indices, [&mesh](uint32_t index) { return index >= mesh->vertices.size(); });
		it != mesh->indices.end())
		report.Fail("index {} out of range ({} vertices)", *it, mesh->vertices.size());

	uint32_t submeshIndices = 0;
	for (const auto& submesh : mesh->submeshes)
	{
		if (submesh.firstIndex != submeshIndices)
			report.Fail("submeshes are not contiguous at index {}", submesh.firstIndex);
		submeshIndices = submesh.firstIndex + submesh.indexCount;
		if (submesh.material >= static_cast<int32_t>(mesh->materials.size()))
			report.Fail("submesh material {} out of range", submesh.material);
		if (auto size = IndicesPerPrimitive(submesh.topology); submesh.indexCount % size != 0)
			report.Fail("submesh index count {} is not a multiple of {}", submesh.indexCount, size);
	}

	// the triangle submeshes' indices (lines and points are only checked for their indices and vertices). vertices of
	// lines and points may have zero normals (none in the file: drawn unlit), those of triangles may not
	std::vector<uint32_t> triangleIndices;
	for (const auto& submesh : mesh->submeshes)
		if (submesh.topology == rhi::PrimitiveTopology::kTriangleList && submesh.firstIndex + submesh.indexCount <= mesh->indices.size())
			triangleIndices.insert(
				triangleIndices.end(), mesh->indices.begin() + submesh.firstIndex,
				mesh->indices.begin() + submesh.firstIndex + submesh.indexCount);
	std::vector<bool> inTriangles(mesh->vertices.size(), false);
	for (auto index : triangleIndices)
		if (index < inTriangles.size())
			inTriangles[index] = true;
	if (submeshIndices != mesh->indices.size())
		report.Fail("submeshes cover {} of {} indices", submeshIndices, mesh->indices.size());

	size_t nonFinite = 0;
	size_t badNormals = 0;
	size_t badTangents = 0; // not unit length, or w not +-1 (0, no tangent, is fine)
	size_t skewedTangents = 0; // far from perpendicular to the normal
	for (size_t vertexIt = 0; vertexIt < mesh->vertices.size(); vertexIt++)
	{
		const auto& vertex = mesh->vertices[vertexIt];
		auto values = {
			vertex.position[0], vertex.position[1], vertex.position[2],
			vertex.normal[0], vertex.normal[1], vertex.normal[2],
			vertex.tangent[0], vertex.tangent[1], vertex.tangent[2], vertex.tangent[3],
			vertex.texCoord01[0], vertex.texCoord01[1],
			vertex.color[0], vertex.color[1], vertex.color[2], vertex.color[3]};
		if (std::ranges::any_of(values, [](float v) { return !std::isfinite(v); }))
			nonFinite++;

		auto n = ToVec3(vertex.normal);
		auto normalLength = std::sqrt((n[0] * n[0]) + (n[1] * n[1]) + (n[2] * n[2]));
		if (std::abs(normalLength - 1.0) > 1e-3 && (inTriangles[vertexIt] || normalLength != 0.0))
			badNormals++;

		if (vertex.tangent[3] != 0.0F)
		{
			Vec3 t{vertex.tangent[0], vertex.tangent[1], vertex.tangent[2]};
			auto length = std::sqrt((t[0] * t[0]) + (t[1] * t[1]) + (t[2] * t[2]));
			if (std::abs(length - 1.0) > 1e-3 || std::abs(vertex.tangent[3]) != 1.0F)
				badTangents++;
			else if (std::abs((t[0] * n[0]) + (t[1] * n[1]) + (t[2] * n[2])) > 0.5)
				skewedTangents++;
		}
	}
	if (badTangents > 0)
		report.Fail("{} tangents are not unit length with w = +-1", badTangents);
	// the shader makes them perpendicular (and uses the derived frame for parallel ones), but far off they say little
	// about the surface. a property of the file, not of the importer
	if (skewedTangents > 0)
		report.Warn("{} tangents are more than 30 degrees from perpendicular to their normal", skewedTangents);
	if (nonFinite > 0)
		report.Fail("{} vertices have non-finite values", nonFinite);
	if (badNormals > 0)
		report.Fail("{} vertices have normals that aren't unit length (or zero, for lines and points)", badNormals);

	// what the triangles' geometric (counter-clockwise) normals say about the vertex normals and the up axis
	std::array<double, 6> axisArea{}; // area facing +x, -x, +y, -y, +z, -z
	double agreeing = 0.0;
	double total = 0.0;
	double signedVolume = 0.0; // positive if the counter-clockwise side faces out, for closed meshes
	for (size_t i = 0; i + 2 < triangleIndices.size(); i += 3)
	{
		const auto& v0 = mesh->vertices[triangleIndices[i]];
		const auto& v1 = mesh->vertices[triangleIndices[i + 1]];
		const auto& v2 = mesh->vertices[triangleIndices[i + 2]];
		auto p0 = ToVec3(v0.position);
		auto p1 = ToVec3(v1.position);
		auto p2 = ToVec3(v2.position);
		Vec3 e1{p1[0] - p0[0], p1[1] - p0[1], p1[2] - p0[2]};
		Vec3 e2{p2[0] - p0[0], p2[1] - p0[1], p2[2] - p0[2]};
		Vec3 c{(e1[1] * e2[2]) - (e1[2] * e2[1]), (e1[2] * e2[0]) - (e1[0] * e2[2]), (e1[0] * e2[1]) - (e1[1] * e2[0])};
		auto area = std::sqrt((c[0] * c[0]) + (c[1] * c[1]) + (c[2] * c[2]));
		if (!(area > 0.0))
			continue;

		auto center = mesh->bounds.Center();
		Vec3 q0{p0[0] - center.x, p0[1] - center.y, p0[2] - center.z};
		signedVolume += ((q0[0] * c[0]) + (q0[1] * c[1]) + (q0[2] * c[2])) / 6.0;

		Vec3 n{};
		for (const auto* v : {&v0, &v1, &v2})
			for (size_t k = 0; k < 3; k++)
				n[k] += v->normal[k];
		total += area;
		if ((c[0] * n[0]) + (c[1] * n[1]) + (c[2] * n[2]) > 0.0)
			agreeing += area;

		for (size_t k = 0; k < 3; k++)
		{
			auto cosine = c[k] / area;
			if (cosine > 0.9)
				axisArea[2 * k] += area;
			else if (cosine < -0.9)
				axisArea[(2 * k) + 1] += area;
		}
	}

	// stray vertices: far away from the rest, which a broken export leaves (spikes, and bounds, and so a camera framing,
	// far larger than the model). measured from the median point, which they don't drag along as they do the bounds. at
	// rest, in world space: instanced submeshes keep their vertices in their node's space (by their first instance), and
	// skinned ones in their mesh's (by their joints at rest).
	if (!mesh->vertices.empty())
	{
		std::vector<std::array<double, 3>> positions(mesh->vertices.size());
		for (size_t vertexIt = 0; vertexIt < mesh->vertices.size(); vertexIt++)
			for (size_t axis = 0; axis < 3; axis++)
				positions[vertexIt][axis] = mesh->vertices[vertexIt].position[axis];
		auto restJoints = mesh->skinVertices.empty() ? std::vector<gfx::SceneMatrix>{} : gfx::RestJoints(mesh->animation);
		for (const auto& submesh : mesh->submeshes)
		{
			if (submesh.firstIndex + submesh.indexCount > mesh->indices.size())
				continue;
			const auto& transform = mesh->instances[std::min<size_t>(submesh.firstInstance, mesh->instances.size() - 1)];
			for (auto index : std::span(mesh->indices).subspan(submesh.firstIndex, submesh.indexCount))
			{
				if (index >= positions.size())
					continue;
				auto position = std::to_array(mesh->vertices[index].position);
				if (submesh.skin >= 0 && !mesh->skinVertices.empty())
					position = gfx::SkinPosition(restJoints, mesh->animation.skins[submesh.skin].jointBase, mesh->skinVertices[index], position);
				for (size_t axis = 0; axis < 3; axis++)
					positions[index][axis] = (transform[axis] * position[0]) + (transform[4 + axis] * position[1]) +
											 (transform[8 + axis] * position[2]) + transform[12 + axis];
			}
		}

		auto medianOf = [](std::vector<double> values)
		{
			auto middle = values.begin() + static_cast<std::ptrdiff_t>(values.size() / 2);
			std::ranges::nth_element(values, middle);
			return *middle;
		};
		Vec3 median{};
		std::vector<double> values(positions.size());
		for (size_t axis = 0; axis < 3; axis++)
		{
			std::ranges::transform(positions, values.begin(), [axis](const auto& position) { return position[axis]; });
			median[axis] = medianOf(values);
		}
		std::ranges::transform(positions, values.begin(), [&median](const auto& position)
		{
			Vec3 d{position[0] - median[0], position[1] - median[1], position[2] - median[2]};
			return std::sqrt((d[0] * d[0]) + (d[1] * d[1]) + (d[2] * d[2]));
		});
		auto medianDistance = medianOf(values);
		constexpr double kStrayFactor = 100.0; // large ground planes and backdrops reach about 20 (mori_knob)
		auto stray = std::ranges::count_if(values, [medianDistance](double d) { return d > kStrayFactor * medianDistance; });
		if (medianDistance > 0.0 && stray > 0)
			report.Warn(
				"{} vertices are more than {:.0f} times as far from the median point as the median vertex (stray vertices?)",
				stray, kStrayFactor);
	}

	auto size = mesh->bounds.Size();
	if (auto boundsVolume = static_cast<double>(size.x) * size.y * size.z; boundsVolume > 0.0)
		report.Info("signed volume {:.3g} of bounds", signedVolume / boundsVolume);

	if (stats.flippedParts > 0)
		report.Info("{} parts were wound clockwise relative to their normals, and were flipped", stats.flippedParts);

	if (stats.windingAgreement >= 0.0)
	{
		report.Info("winding agrees with file normals: {:.1f}%", 100.0 * stats.windingAgreement);
		if (stats.windingAgreement < 0.5)
			report.Warn("winding is mostly clockwise relative to the file's normals (flipped winding or normals)");
		else if (stats.windingAgreement < 0.9)
			report.Warn("winding disagrees with the file's normals on {:.1f}% of the area", 100.0 * (1.0 - stats.windingAgreement));
	}
	// generated normals follow the winding, so this checks the importer rather than the file
	if (!mesh->hasNormals && total > 0.0 && agreeing / total < 0.99)
		report.Fail("winding disagrees with the generated normals on {:.1f}% of the area", 100.0 * (1.0 - (agreeing / total)));

	// floors, ceilings and the flat bottoms of objects are horizontal: if much of the area faces along an axis (either
	// way), the axis with the most is likely vertical
	static constexpr std::array<std::string_view, 3> kAxes{"x", "y", "z"};
	std::array<double, 3> pairArea{axisArea[0] + axisArea[1], axisArea[2] + axisArea[3], axisArea[4] + axisArea[5]};
	auto verticalIt = std::ranges::max_element(pairArea) - pairArea.begin();
	double axisAligned = pairArea[0] + pairArea[1] + pairArea[2];
	if (total > 0.0 && axisAligned / total > 0.2)
	{
		report.Info(
			"axis aligned area: x {:.0f}%, y {:.0f}% (up {:.0f}%), z {:.0f}%", 100.0 * pairArea[0] / axisAligned,
			100.0 * pairArea[1] / axisAligned, 100.0 * axisArea[2] / axisAligned, 100.0 * pairArea[2] / axisAligned);
		// only a hint: the long walls of a building or the sides of a flat object can outweigh the floors
		if (verticalIt != 1 && pairArea[verticalIt] > 1.5 * pairArea[1])
			report.Info("{} may be the vertical axis rather than y", kAxes[verticalIt]);
	}

	const auto& min = mesh->bounds.GetMin();
	const auto& max = mesh->bounds.GetMax();
	report.Info("bounds [{:.3g} {:.3g} {:.3g}] - [{:.3g} {:.3g} {:.3g}]", min.x, min.y, min.z, max.x, max.y, max.z);

	auto vertexBytes = mesh->vertices.size() * sizeof(VertexP3fN3fTa4fT014fC4f);
	report.Info("vertex buffer {:.1f} MiB, index buffer {:.1f} MiB", vertexBytes / 1048576.0, mesh->indices.size() * 4 / 1048576.0);

	if (stats.droppedTriangles > 0)
		report.Warn("{} triangles dropped (invalid vertex index)", stats.droppedTriangles);
	if (stats.degenerateTriangles > 0)
		report.Info("{} degenerate triangles", stats.degenerateTriangles);
	if (stats.repairedNormals > 0)
		report.Warn("{} unusable normals in the file replaced", stats.repairedNormals);
	if (stats.generatedTangents > 0)
		report.Info("{} vertices with generated (MikkTSpace) tangents", stats.generatedTangents);
	if (stats.invalidTangents > 0)
		report.Warn("{} unusable tangents in the file, left to the shader", stats.invalidTangents);
	if (stats.nonFiniteValues > 0)
		report.Warn("{} non-finite values in the file replaced", stats.nonFiniteValues);
	if (stats.missingTextures > 0)
		report.Warn("{} textures not found", stats.missingTextures);
	for (const auto& warning : stats.warnings)
		report.Info("parser: {}", warning);

	// with the usage the client loads them with
	for (const auto& material : mesh->materials)
	{
		if (!material.diffuseTexture.empty())
			Add(texturesOut, material.diffuseTexture, gfx::image::Usage::kColor, 1.0F);
		if (!material.alphaTexture.empty())
			Add(texturesOut, material.alphaTexture, gfx::image::Usage::kMask, 1.0F);
		if (!material.occlusionTexture.empty())
			Add(texturesOut, material.occlusionTexture, gfx::image::Usage::kOcclusion, 1.0F);
		if (!material.metallicRoughnessTexture.empty())
			Add(texturesOut, material.metallicRoughnessTexture, gfx::image::Usage::kMetallicRoughness, 1.0F);
		if (!material.specularTexture.empty())
			Add(texturesOut, material.specularTexture, gfx::image::Usage::kAlpha, 1.0F);
		if (!material.specularColorTexture.empty())
			Add(texturesOut, material.specularColorTexture, gfx::image::Usage::kColor, 1.0F);
		if (!material.clearcoatTexture.empty())
			Add(texturesOut, material.clearcoatTexture, gfx::image::Usage::kOcclusion, 1.0F);
		if (!material.clearcoatRoughnessTexture.empty())
			Add(texturesOut, material.clearcoatRoughnessTexture, gfx::image::Usage::kMetallicRoughness, 1.0F);
		if (!material.clearcoatNormalTexture.empty())
			Add(texturesOut, material.clearcoatNormalTexture, gfx::image::Usage::kNormal, 1.0F);
		if (!material.sheenColorTexture.empty())
			Add(texturesOut, material.sheenColorTexture, gfx::image::Usage::kColor, 1.0F);
		if (!material.sheenRoughnessTexture.empty())
			Add(texturesOut, material.sheenRoughnessTexture, gfx::image::Usage::kAlpha, 1.0F);
		if (!material.transmissionTexture.empty())
			Add(texturesOut, material.transmissionTexture, gfx::image::Usage::kOcclusion, 1.0F);
		if (!material.thicknessTexture.empty())
			Add(texturesOut, material.thicknessTexture, gfx::image::Usage::kMetallicRoughness, 1.0F);
		if (!material.anisotropyTexture.empty())
			Add(texturesOut, material.anisotropyTexture, gfx::image::Usage::kLinear, 1.0F);
		if (!material.iridescenceTexture.empty())
			Add(texturesOut, material.iridescenceTexture, gfx::image::Usage::kOcclusion, 1.0F);
		if (!material.iridescenceThicknessTexture.empty())
			Add(texturesOut, material.iridescenceThicknessTexture, gfx::image::Usage::kMetallicRoughness, 1.0F);
		if (!material.diffuseTransmissionTexture.empty())
			Add(texturesOut, material.diffuseTransmissionTexture, gfx::image::Usage::kAlpha, 1.0F);
		if (!material.diffuseTransmissionColorTexture.empty())
			Add(texturesOut, material.diffuseTransmissionColorTexture, gfx::image::Usage::kColor, 1.0F);
		if (!material.normalTexture.empty())
			Add(texturesOut, material.normalTexture, gfx::image::Usage::kNormal, 1.0F);
		else if (!material.bumpTexture.empty())
			Add(texturesOut, material.bumpTexture, gfx::image::Usage::kBump, material.bumpScale);
	}

	return report;
}

// the encodings of one model should import to the same mesh: files named after the model's directory, in its
// subdirectories, as the gltf sample models come (Duck/glTF/Duck.gltf, Duck/glTF-Binary/Duck.glb, ...). they only warn if they don't, since a
// variant can legitimately be another export (ABeautifulGame's glb has 1152 fewer triangles than its gltf).
Report CheckEncodings(const std::vector<std::pair<std::filesystem::path, ModelSummary>>& encodings)
{
	Report report;

	auto variant = [](const std::filesystem::path& path) { return path.parent_path().filename().string(); };
	std::string names;
	for (const auto& [path, summary] : encodings)
		names += (names.empty() ? "" : ", ") + variant(path);
	report.Info("{} encodings: {}", encodings.size(), names);

	auto compare = [&](std::string_view what, auto field)
	{
		const auto& [firstPath, first] = encodings.front();
		std::string values;
		bool differ = false;
		for (const auto& [path, summary] : encodings)
		{
			differ |= field(summary) != field(first);
			values += std::format("{}{} {}", values.empty() ? "" : ", ", variant(path), field(summary));
		}
		if (differ)
			report.Warn("{} differ: {}", what, values);
	};
	compare("triangle counts", [](const ModelSummary& summary) { return summary.triangles; });
	compare("vertex counts", [](const ModelSummary& summary) { return summary.vertices; });
	compare("material counts", [](const ModelSummary& summary) { return summary.materials; });
	compare("submesh counts", [](const ModelSummary& summary) { return summary.submeshes; });
	compare("texture counts", [](const ModelSummary& summary) { return summary.textures; });

	// within 1% of the extent: quantized encodings (KHR_mesh_quantization) round positions
	const auto& first = encodings.front().second;
	double extent = 0.0;
	for (size_t axis = 0; axis < 3; axis++)
		extent = std::max(extent, first.max[axis] - first.min[axis]);
	for (const auto& [path, summary] : encodings)
	{
		double offBy = 0.0;
		for (size_t axis = 0; axis < 3; axis++)
			offBy = std::max({offBy, std::abs(summary.min[axis] - first.min[axis]), std::abs(summary.max[axis] - first.max[axis])});
		if (offBy > 0.01 * extent)
			report.Warn("{}'s bounds differ from {}'s by {:.3g} ({:.1f}% of the extent)", variant(path), variant(encodings.front().first), offBy, 100.0 * offBy / extent);
	}

	return report;
}

constexpr std::string_view ToString(gfx::image::Format format)
{
	switch (format)
	{
	case gfx::image::Format::kBC1: return "BC1";
	case gfx::image::Format::kBC3: return "BC3";
	case gfx::image::Format::kBC4: return "BC4";
	case gfx::image::Format::kBC5: return "BC5";
	case gfx::image::Format::kBC7: return "BC7";
	}
	return "?";
}

constexpr std::string_view ToString(gfx::image::Usage usage)
{
	switch (usage)
	{
	case gfx::image::Usage::kColor: return "color";
	case gfx::image::Usage::kLinear: return "linear";
	case gfx::image::Usage::kNormal: return "normal";
	case gfx::image::Usage::kBump: return "bump";
	case gfx::image::Usage::kMask: return "mask";
	case gfx::image::Usage::kOcclusion: return "occlusion";
	case gfx::image::Usage::kMetallicRoughness: return "metallic-roughness";
	case gfx::image::Usage::kAlpha: return "alpha";
	}
	return "?";
}

Report CheckImage(const std::filesystem::path& path, std::optional<uint32_t> embeddedImage, const gfx::image::Options& options)
{
	using gfx::image::Usage;

	Report report;

	// the image's bytes: its file's, or a gltf file's embedded image
	std::vector<std::byte> bytes;
	if (embeddedImage)
	{
		auto embedded = gfx::gltf::EmbeddedImage(path, *embeddedImage);
		if (!embedded)
		{
			report.Fail("{}", embedded.error());
			return report;
		}
		bytes = std::move(*embedded);
	}
	else
	{
		std::ifstream file(path, std::ios::binary);
		std::vector<char> chars((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
		bytes.resize(chars.size());
		std::memcpy(bytes.data(), chars.data(), chars.size());
	}
	auto name = embeddedImage ? std::format("{}#image{}", path.string(), *embeddedImage) : path.string();

	static constexpr std::byte kFill{0xcd};
	std::vector<std::byte> data;
	auto image = gfx::image::Import(
		bytes,
		name,
		options,
		[&data](size_t size)
		{
			data.assign(size, kFill);
			return data.data();
		});
	if (!image)
	{
		report.Fail("{}", image.error());
		return report;
	}

	// what Import compressed: the image prepared for the usage
	auto reference = gfx::image::Decode(bytes, name, options);
	if (!reference)
	{
		report.Fail("reference decode failed: {}", reference.error());
		return report;
	}
	auto width = reference->width;
	auto height = reference->height;
	const auto& pixels = reference->rgba;

	auto format = image->format;
	// a file's own blocks and mips (KTX2) only warn: their quality is the file's (and libktx's transcoder's), not the
	// importer's
	auto failOrWarn = [&report, ownBlocks = image->ownBlocks](const std::string& what)
	{
		if (ownBlocks)
			report.Warn("{} (the file's own blocks and mips)", what);
		else
			report.Fail("{}", what);
	};
	report.Info(
		"{}x{}, {} channels, as {}{}: {}, {} mips", width, height, reference->channelCount, ToString(options.usage),
		image->fromHeight ? std::format(" (from a height map, scale {})", options.bumpScale) : "", ToString(format),
		image->mipLevels.size());

	auto expectedLevels = static_cast<size_t>(std::bit_width(std::max(width, height)));
	if (image->mipLevels.size() != expectedLevels)
		report.Fail("{} mip levels, expected {}", image->mipLevels.size(), expectedLevels);

	auto blockSize = gfx::image::BlockSize(format);
	size_t offset = 0;
	for (size_t levelIt = 0; levelIt < image->mipLevels.size(); levelIt++)
	{
		const auto& level = image->mipLevels[levelIt];
		auto expectedWidth = std::max(width >> levelIt, 1U);
		auto expectedHeight = std::max(height >> levelIt, 1U);
		if (level.width != expectedWidth || level.height != expectedHeight)
			report.Fail("mip {} is {}x{}, expected {}x{}", levelIt, level.width, level.height, expectedWidth, expectedHeight);
		auto expectedSize = ((level.width + 3) / 4) * ((level.height + 3) / 4) * blockSize;
		if (level.size != expectedSize || level.offset != offset)
			report.Fail("mip {} has size {} at {}, expected {} at {}", levelIt, level.size, level.offset, expectedSize, offset);
		offset += level.size;
	}
	if (offset != image->size || data.size() != image->size)
		report.Fail("size {} (allocated {}), mips add up to {}", image->size, data.size(), offset);

	// any block still holding the fill pattern was never written
	size_t unwritten = 0;
	for (size_t blockIt = 0; blockIt + blockSize <= data.size(); blockIt += blockSize)
		if (std::all_of(&data[blockIt], &data[blockIt] + blockSize, [](std::byte b) { return b == kFill; }))
			unwritten++;
	if (unwritten > 0)
		report.Fail("{} blocks were not written", unwritten);

	if (image->mipLevels.empty() || unwritten > 0)
		return report;

	// decode the blocks back, and compare level 0 to the reference, and the average of each level to level 0's
	auto decodeLevel = [&](const gfx::image::MipLevel& level)
	{
		std::vector<uint8_t> rgba(static_cast<size_t>(level.width) * level.height * 4);
		auto blockCols = (level.width + 3) / 4;
		std::array<uint8_t, 64> block{};
		for (uint32_t by = 0; by < (level.height + 3) / 4; by++)
		{
			for (uint32_t bx = 0; bx < blockCols; bx++)
			{
				auto compressed = std::span(data).subspan(level.offset + (((by * blockCols) + bx) * blockSize), blockSize);
				// bc7 (only from KTX2 files) with bcdec, the others as gfx::image decodes them
				if (format == gfx::image::Format::kBC7)
					bcdec_bc7(compressed.data(), block.data(), 4 * 4);
				else
					gfx::image::DecompressBlock(format, compressed, block);
				for (uint32_t y = 0; y < 4 && (by * 4) + y < level.height; y++)
					for (uint32_t x = 0; x < 4 && (bx * 4) + x < level.width; x++)
						std::copy_n(&block[((y * 4) + x) * 4], 4, &rgba[((((by * 4) + y) * level.width) + (bx * 4) + x) * 4]);
			}
		}
		return rgba;
	};

	auto level0 = decodeLevel(image->mipLevels[0]);
	auto pixelCount = static_cast<size_t>(width) * height;

	// the channels the format holds: rgb, r (bc4), or x and y (bc5)
	size_t channelCount = format == gfx::image::Format::kBC4 ? 1 : format == gfx::image::Format::kBC5 ? 2 : 3;
	double squaredError = 0.0;
	for (size_t i = 0; i < pixelCount; i++)
	{
		for (size_t ch = 0; ch < channelCount; ch++)
		{
			double d = static_cast<double>(level0[(i * 4) + ch]) - pixels[(i * 4) + ch];
			squaredError += d * d;
		}
	}
	auto mse = squaredError / static_cast<double>(pixelCount * channelCount);
	auto psnr = mse > 0.0 ? 10.0 * std::log10(255.0 * 255.0 / mse) : 99.0;
	report.Info("level 0 psnr {:.1f} dB", psnr);
	// bc1 can't do much better on noisy textures
	if (psnr < 18.0)
		failOrWarn(std::format("level 0 psnr {:.1f} dB is too low", psnr));
	else if (psnr < 22.0)
		report.Warn("level 0 psnr {:.1f} dB is low", psnr);

	auto decodeSigned = [](uint8_t value) { return (value / 255.0 * 2.0) - 1.0; };

	if (options.usage == Usage::kNormal || options.usage == Usage::kBump)
	{
		// the angle between the reconstructed normals and the reference ones, and how many point below the surface. bc5
		// keeps x and y, and z is reconstructed as positive: a reference normal below the surface (z < 0, which a valid
		// normal map doesn't have) comes back mirrored, so it is compared mirrored, and counted.
		double angleSum = 0.0;
		double maxAngle = 0.0;
		size_t belowSurface = 0;
		for (size_t i = 0; i < pixelCount; i++)
		{
			std::array<double, 3> a{};
			std::array<double, 3> b{};
			for (size_t ch = 0; ch < 3; ch++)
			{
				a[ch] = decodeSigned(level0[(i * 4) + ch]);
				b[ch] = decodeSigned(pixels[(i * 4) + ch]);
			}
			if (b[2] < 0.0)
			{
				belowSurface++;
				b[2] = -b[2];
			}
			auto dot = (a[0] * b[0]) + (a[1] * b[1]) + (a[2] * b[2]);
			auto lengths = std::sqrt(((a[0] * a[0]) + (a[1] * a[1]) + (a[2] * a[2])) * ((b[0] * b[0]) + (b[1] * b[1]) + (b[2] * b[2])));
			auto angle = std::acos(std::clamp(lengths > 0.0 ? dot / lengths : 1.0, -1.0, 1.0)) * 180.0 / std::numbers::pi;
			angleSum += angle;
			maxAngle = std::max(maxAngle, angle);
		}
		auto meanAngle = angleSum / static_cast<double>(pixelCount);
		report.Info("level 0 normals off by {:.2f} degrees on average, {:.1f} at most", meanAngle, maxAngle);
		if (auto share = static_cast<double>(belowSurface) / static_cast<double>(pixelCount); share > 0.01)
			report.Warn("{:.1f}% of the source normals point below the surface, and are drawn mirrored", 100.0 * share);
		if (meanAngle > 5.0)
			report.Fail("level 0 normals are off by {:.2f} degrees on average", meanAngle);
		return report;
	}

	if (options.usage == Usage::kColor || options.usage == Usage::kLinear)
	{
		uint32_t maxAlphaError = 0;
		for (size_t i = 0; i < pixelCount; i++)
		{
			bool hasAlpha = format == gfx::image::Format::kBC3 || format == gfx::image::Format::kBC7;
			auto sourceAlpha = hasAlpha ? pixels[(i * 4) + 3] : 255;
			maxAlphaError = std::max(maxAlphaError, static_cast<uint32_t>(std::abs(level0[(i * 4) + 3] - sourceAlpha)));
		}
		if (maxAlphaError > 24)
			failOrWarn(std::format("max alpha error {} is too high", maxAlphaError));

		// bc7 holds alpha whether the image has it or not
		if (format != gfx::image::Format::kBC7 && reference->alpha != (format == gfx::image::Format::kBC3))
			report.Fail("alpha {} but format is {}", reference->alpha ? "present" : "absent", ToString(format));
	}

	// rgb weighted by alpha (the resize doesn't let the color of transparent pixels bleed into the mips), and alpha. for
	// color in linear space, where the mips are filtered, and back to srgb for comparing.
	bool srgb = options.usage == Usage::kColor;
	auto toLinear = [srgb](double c)
	{
		c /= 255.0;
		return !srgb ? c : c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
	};
	auto fromLinear = [srgb](double c)
	{
		return 255.0 * (!srgb ? c : c <= 0.0031308 ? c * 12.92 : (1.055 * std::pow(c, 1.0 / 2.4)) - 0.055);
	};
	auto average = [&toLinear, &fromLinear](const std::vector<uint8_t>& rgba)
	{
		std::array<double, 4> sum{};
		for (size_t i = 0; i < rgba.size(); i += 4)
		{
			for (size_t ch = 0; ch < 3; ch++)
				sum[ch] += toLinear(rgba[i + ch]) * (rgba[i + 3] / 255.0);
			sum[3] += rgba[i + 3];
		}
		for (size_t ch = 0; ch < 3; ch++)
			sum[ch] = sum[3] > 0.0 ? fromLinear(sum[ch] * 255.0 / sum[3]) : 0.0;
		sum[3] /= static_cast<double>(rgba.size() / 4);
		return sum;
	};

	// a level of one block (4x4 or smaller) holds at most four colors, on a line for bc1: one whose pixels have more
	// distinct colors than that can't keep their average, however well it is filtered, so it only warns
	auto levelAverage0 = average(level0);
	double worst = 0.0;
	size_t worstLevel = 0;
	double worstSingleBlock = 0.0;
	size_t worstSingleBlockLevel = 0;
	for (size_t levelIt = 1; levelIt < image->mipLevels.size(); levelIt++)
	{
		const auto& level = image->mipLevels[levelIt];
		bool singleBlock = level.width <= 4 && level.height <= 4;
		auto levelAverage = average(decodeLevel(level));
		for (size_t ch = 0; ch < 4; ch++)
		{
			// only the channels the format holds, and alpha: bc5's blue is a normal's z, reconstructed from x and y,
			// whose average doesn't carry over to the mips for what isn't a normal map (e.g. metallic-roughness)
			if (ch < 3 && ch >= channelCount)
				continue;
			auto d = std::abs(levelAverage[ch] - levelAverage0[ch]);
			auto& levelWorst = singleBlock ? worstSingleBlock : worst;
			if (d > levelWorst)
			{
				levelWorst = d;
				(singleBlock ? worstSingleBlockLevel : worstLevel) = levelIt;
			}
		}
	}
	// odd extents drop a row or column per level, so small drifts are expected
	if (worst > 24.0)
		failOrWarn(std::format("mip {} average is off by {:.1f}", worstLevel, worst));
	else if (worst > 12.0)
		report.Warn("mip {} average is off by {:.1f}", worstLevel, worst);
	if (worstSingleBlock > 12.0)
		report.Warn("mip {} (a single block) average is off by {:.1f}", worstSingleBlockLevel, worstSingleBlock);

	return report;
}

// prefilters an environment panorama and checks the result: every level there and finite, each level's mean radiance as the
// panorama's (filtering moves light around, it doesn't add or remove any), and the irradiance's mean over the sphere
// (its constant term) the panorama's mean too
Report CheckEnvironment(const gfx::environment::Panorama& panorama)
{
	using namespace gfx::environment;

	Report report;
	std::vector<std::byte> data;
	auto environment = Prefilter(panorama, [&data](size_t size) { data.resize(size); return data.data(); });
	if (!environment)
	{
		report.Fail("{}", environment.error());
		return report;
	}

	auto luminance = [](const std::array<double, 3>& rgb) { return (0.2126 * rgb[0]) + (0.7152 * rgb[1]) + (0.0722 * rgb[2]); };
	auto mean = luminance(MeanRadiance(panorama));
	report.Info("{}x{}, mean radiance {:.4g}", panorama.width, panorama.height, mean);
	if (!(mean > 0.0) || !std::isfinite(mean))
	{
		report.Fail("the panorama is black, or not finite");
		return report;
	}

	// how much a level varies (the standard deviation of its luminance, by solid angle): rougher levels are smoother
	auto deviation = [&data, &luminance](const gfx::image::MipLevel& level)
	{
		const auto* texels = reinterpret_cast<const uint16_t*>(data.data() + level.offset);
		double sum = 0.0;
		double sum2 = 0.0;
		double weight = 0.0;
		for (uint32_t y = 0; y < level.height; y++)
		{
			double w = std::sin((y + 0.5) / level.height * std::numbers::pi);
			for (uint32_t x = 0; x < level.width; x++)
			{
				const auto* texel = &texels[(static_cast<size_t>(y) * level.width + x) * 4];
				auto l = luminance({glm::unpackHalf1x16(texel[0]), glm::unpackHalf1x16(texel[1]), glm::unpackHalf1x16(texel[2])});
				sum += l * w;
				sum2 += l * l * w;
				weight += w;
			}
		}
		auto mean = sum / weight;
		return std::sqrt(std::max(sum2 / weight - mean * mean, 0.0));
	};
	std::vector<double> deviations;
	for (const auto& level : environment->levels)
		deviations.push_back(deviation(level));
	report.Info("level deviations {}", deviations);
	for (size_t levelIt = 1; levelIt < deviations.size(); levelIt++)
		if (deviations[levelIt] > deviations[levelIt - 1] * 1.02)
			report.Fail("level {} varies more than level {} ({:.4g} > {:.4g}): it isn't filtered", levelIt, levelIt - 1, deviations[levelIt], deviations[levelIt - 1]);

	if (environment->levels.size() != kLevelCount || environment->size != data.size())
		report.Fail("{} levels of {} bytes, expected {} of {}", environment->levels.size(), data.size(), kLevelCount, environment->size);
	for (size_t levelIt = 0; levelIt < environment->levels.size(); levelIt++)
	{
		const auto& level = environment->levels[levelIt];
		auto levelMean = luminance(MeanRadiance(level, data));
		auto error = std::abs(levelMean / mean - 1.0);
		if (!std::isfinite(levelMean))
			report.Fail("level {} has values that aren't finite", levelIt);
		else if (error > 0.1)
			report.Fail("level {} ({}x{}) mean radiance {:.4g} is off by {:.1f}%", levelIt, level.width, level.height, levelMean, error * 100.0);
		else if (error > 0.03)
			report.Warn("level {} ({}x{}) mean radiance {:.4g} is off by {:.1f}%", levelIt, level.width, level.height, levelMean, error * 100.0);
	}

	// the constant band, times its basis function, is the mean over the sphere
	constexpr double kY00 = 0.282095;
	std::array<double, 3> irradianceMean{};
	for (size_t c = 0; c < 3; c++)
		irradianceMean[c] = environment->irradiance[0][c] * kY00;
	if (auto error = std::abs(luminance(irradianceMean) / mean - 1.0); error > 0.02)
		report.Fail("the irradiance's mean {:.4g} is off by {:.1f}%", luminance(irradianceMean), error * 100.0);
	auto up = EvaluateIrradiance(*environment, {0.0F, 1.0F, 0.0F});
	auto down = EvaluateIrradiance(*environment, {0.0F, -1.0F, 0.0F});
	report.Info("irradiance up {:.4g}, down {:.4g}", luminance({up[0], up[1], up[2]}), luminance({down[0], down[1], down[2]}));

	return report;
}

void Print(const std::filesystem::path& path, const Report& report, std::chrono::duration<double> time)
{
	std::println("{} {} ({:.2f}s)", ToString(report.result), path.string(), time.count());
	for (const auto& note : report.notes)
		std::println("    {}", note);
}

} // namespace

int main(int argc, char* argv[])
{
	bool models = true;
	bool images = true;
	bool archiveFailed = false;
	std::vector<std::filesystem::path> modelFiles;
	std::vector<std::filesystem::path> environmentFiles;
	ImageChecks imageFiles;

	for (int argIt = 1; argIt < argc; argIt++)
	{
		std::string_view arg = argv[argIt]; //NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
		if (arg == "--models-only")
		{
			images = false;
			continue;
		}
		if (arg == "--images-only")
		{
			models = false;
			continue;
		}

		std::filesystem::path path(arg);

		// a zip archive is extracted (as the client does) to a temporary directory, which is then searched
		if (Lower(path.extension().string()) == ".zip")
		{
			auto directory = std::filesystem::temp_directory_path() / "assettest" / path.stem();
			std::error_code error;
			std::filesystem::remove_all(directory, error);
			auto start = std::chrono::steady_clock::now();
			if (auto result = gfx::zip::ExtractAll(path, directory); !result)
			{
				std::println("FAIL {}\n    {}", path.string(), result.error());
				archiveFailed = true;
				continue;
			}
			std::println(
				"PASS {} ({:.2f}s)\n    extracted to {}", path.string(),
				std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count(), directory.string());
			path = directory;
		}

		if (std::filesystem::is_directory(path))
		{
			for (auto it = std::filesystem::recursive_directory_iterator(path); it != std::filesystem::recursive_directory_iterator(); ++it)
			{
				const auto& entry = *it;
				// macOS resource forks, which zip archives made on macs often carry
				if (entry.path().filename() == "__MACOSX")
				{
					it.disable_recursion_pending();
					continue;
				}
				if (!entry.is_regular_file() || entry.path().filename().string().starts_with("._"))
					continue;
				if (IsModel(entry.path()))
					modelFiles.push_back(entry.path());
				else if (IsEnvironment(entry.path()))
					environmentFiles.push_back(entry.path());
				else if (IsImage(entry.path()))
					Add(imageFiles, std::filesystem::weakly_canonical(entry.path()), std::nullopt, gfx::image::Usage::kColor, 1.0F);
			}
		}
		else if (IsModel(path))
		{
			modelFiles.push_back(path);
		}
		else if (IsEnvironment(path))
		{
			environmentFiles.push_back(path);
		}
		else if (IsImage(path))
		{
			Add(imageFiles, std::filesystem::weakly_canonical(path), std::nullopt, gfx::image::Usage::kColor, 1.0F);
		}
		else
		{
			std::println(stderr, "not a model, image or directory: {}", arg);
			return 2;
		}
	}

	std::ranges::sort(modelFiles);

	ResultCounts modelResults{};
	ResultCounts encodingResults{};
	ResultCounts imageResults{};

	if (models)
	{
		// by the directory above theirs and their (lower case) name: the encodings of a model
		core::UnorderedMap<std::string, std::vector<std::pair<std::filesystem::path, ModelSummary>>> encodings;

		for (const auto& path : modelFiles)
		{
			auto start = std::chrono::steady_clock::now();
			ImageChecks textures;
			std::optional<ModelSummary> summary;
			auto report = CheckModel(path, textures, summary);
			Print(path, report, std::chrono::steady_clock::now() - start);
			Count(modelResults, report.result)++;
			if (images)
				imageFiles.insert(textures.begin(), textures.end());
			if (auto model = path.parent_path().parent_path(); summary && Lower(model.filename().string()) == Lower(path.stem().string()))
				encodings[(model / Lower(path.stem().string())).string()].emplace_back(path, *summary);
		}

		// in order, for a stable report
		std::vector<std::string> models;
		for (const auto& [model, files] : encodings)
			models.push_back(model);
		std::ranges::sort(models);
		for (const auto& model : models)
		{
			const auto& files = encodings[model];
			core::UnorderedSet<std::string> directories;
			for (const auto& [path, summary] : files)
				directories.insert(path.parent_path().string());
			if (files.size() < 2 || directories.size() != files.size())
				continue;

			auto report = CheckEncodings(files);
			Print(model, report, {});
			Count(encodingResults, report.result)++;
		}
	}

	if (images)
	{
		// in order, for a stable report
		std::vector<ImageCheck> checks;
		checks.reserve(imageFiles.size());
		for (const auto& [key, check] : imageFiles)
			checks.push_back(check);
		std::ranges::sort(checks);
		for (const auto& [path, embeddedImage, usage, bumpScale] : checks)
		{
			auto start = std::chrono::steady_clock::now();
			auto report = CheckImage(path, embeddedImage, {.usage = usage, .bumpScale = bumpScale});
			Print(embeddedImage ? std::filesystem::path(std::format("{}#image{}", path.string(), *embeddedImage)) : path, report,
				  std::chrono::steady_clock::now() - start);
			Count(imageResults, report.result)++;
		}
	}

	// the procedural sky (the client's default environment), and the environment files
	ResultCounts environmentResults{};
	if (images)
	{
		auto start = std::chrono::steady_clock::now();
		auto report = CheckEnvironment(gfx::environment::ProceduralSky());
		Print("procedural sky", report, std::chrono::steady_clock::now() - start);
		Count(environmentResults, report.result)++;

		std::ranges::sort(environmentFiles);
		for (const auto& path : environmentFiles)
		{
			start = std::chrono::steady_clock::now();
			auto panorama = gfx::environment::Import(path);
			Report report;
			if (!panorama)
				report.Fail("{}", panorama.error());
			else
				report = CheckEnvironment(*panorama);
			Print(path, report, std::chrono::steady_clock::now() - start);
			Count(environmentResults, report.result)++;
		}
	}

	std::println(
		"models: {} pass, {} warn, {} fail. encodings: {} pass, {} warn. images: {} pass, {} warn, {} fail. environments: {} pass, {} warn, {} fail.",
		Count(modelResults, Result::kPass), Count(modelResults, Result::kWarn), Count(modelResults, Result::kFail),
		Count(encodingResults, Result::kPass), Count(encodingResults, Result::kWarn),
		Count(imageResults, Result::kPass), Count(imageResults, Result::kWarn), Count(imageResults, Result::kFail),
		Count(environmentResults, Result::kPass), Count(environmentResults, Result::kWarn), Count(environmentResults, Result::kFail));

	return archiveFailed ||
				   Count(modelResults, Result::kFail) + Count(imageResults, Result::kFail) + Count(environmentResults, Result::kFail) > 0
			   ? 1
			   : 0;
}
