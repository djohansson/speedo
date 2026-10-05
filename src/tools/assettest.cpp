// imports models and images the way the client does (gfx::mesh::Import, gfx::image::Import) and checks the results.
// usage: assettest [--models-only | --images-only] <file, directory or zip archive>...
// directories (and zip archives, extracted to a temporary directory) are searched recursively for models (.obj, .gltf, .glb) and images. the textures that models' materials name are
// checked too. prints one line per asset (PASS, WARN or FAIL, with details) and a summary, and exits with 1 if any
// asset failed.

#include <gfx/imageimport.h>
#include <gfx/meshimport.h>
#include <gfx/ziparchive.h>

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <format>
#include <map>
#include <numbers>
#include <optional>
#include <print>
#include <set>
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

std::string Lower(std::string str)
{
	std::ranges::transform(str, str.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
	return str;
}

bool IsModel(const std::filesystem::path& path) { return gfx::mesh::IsModelFile(path); }

bool IsImage(const std::filesystem::path& path)
{
	static const std::set<std::string> kExtensions{".png", ".jpg", ".jpeg", ".tga", ".bmp", ".psd", ".gif", ".hdr", ".pic", ".pnm", ".ppm", ".pgm"};
	return kExtensions.contains(Lower(path.extension().string()));
}

using Vec3 = std::array<double, 3>;

Vec3 ToVec3(const float (&v)[3]) { return {v[0], v[1], v[2]}; } //NOLINT(modernize-avoid-c-arrays)

// an image, with the usage it is checked for, and its bump scale for kBump
using ImageCheck = std::tuple<std::filesystem::path, gfx::image::Usage, float>;

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

Report CheckModel(const std::filesystem::path& path, std::set<ImageCheck>& texturesOut, std::optional<ModelSummary>& summaryOut)
{
	Report report;

	// embedded gltf images are extracted (as the client does) to a temporary directory, and checked like the others
	auto mesh = gfx::mesh::Import(
		path, {.embeddedImageDirectory = std::filesystem::temp_directory_path() / "assettest" / "embedded" / path.stem()});
	if (!mesh)
	{
		report.Fail("{}", mesh.error());
		return report;
	}

	const auto& stats = mesh->stats;

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
		"{} triangles, {} vertices, {} materials, {} submeshes, normals: {}, texcoords: {}, colors: {}",
		stats.triangleCount, mesh->vertices.size(), mesh->materials.size(), mesh->submeshes.size(),
		mesh->hasNormals ? "file" : "generated", mesh->hasTexCoords ? "yes" : "no", mesh->hasColors ? "yes" : "no");

	if (mesh->indices.empty())
	{
		report.Fail("no triangles");
		return report;
	}

	if (mesh->indices.size() % 3 != 0)
		report.Fail("index count {} is not a multiple of 3", mesh->indices.size());

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
	}
	if (submeshIndices != mesh->indices.size())
		report.Fail("submeshes cover {} of {} indices", submeshIndices, mesh->indices.size());

	size_t nonFinite = 0;
	size_t badNormals = 0;
	for (const auto& vertex : mesh->vertices)
	{
		auto values = {
			vertex.position[0], vertex.position[1], vertex.position[2],
			vertex.normal[0], vertex.normal[1], vertex.normal[2],
			vertex.texCoord01[0], vertex.texCoord01[1],
			vertex.color[0], vertex.color[1], vertex.color[2], vertex.color[3]};
		if (std::ranges::any_of(values, [](float v) { return !std::isfinite(v); }))
			nonFinite++;

		auto n = ToVec3(vertex.normal);
		if (std::abs(std::sqrt((n[0] * n[0]) + (n[1] * n[1]) + (n[2] * n[2])) - 1.0) > 1e-3)
			badNormals++;
	}
	if (nonFinite > 0)
		report.Fail("{} vertices have non-finite values", nonFinite);
	if (badNormals > 0)
		report.Fail("{} vertices have normals that aren't unit length", badNormals);

	// what the triangles' geometric (counter-clockwise) normals say about the vertex normals and the up axis
	std::array<double, 6> axisArea{}; // area facing +x, -x, +y, -y, +z, -z
	double agreeing = 0.0;
	double total = 0.0;
	double signedVolume = 0.0; // positive if the counter-clockwise side faces out, for closed meshes
	for (size_t i = 0; i + 2 < mesh->indices.size(); i += 3)
	{
		const auto& v0 = mesh->vertices[mesh->indices[i]];
		const auto& v1 = mesh->vertices[mesh->indices[i + 1]];
		const auto& v2 = mesh->vertices[mesh->indices[i + 2]];
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
	// far larger than the model). measured from the median point, which they don't drag along as they do the bounds.
	if (!mesh->vertices.empty())
	{
		auto medianOf = [](std::vector<double>& values)
		{
			auto middle = values.begin() + static_cast<std::ptrdiff_t>(values.size() / 2);
			std::ranges::nth_element(values, middle);
			return *middle;
		};
		Vec3 median{};
		std::vector<double> values(mesh->vertices.size());
		for (size_t axis = 0; axis < 3; axis++)
		{
			std::ranges::transform(mesh->vertices, values.begin(), [axis](const auto& vertex) { return static_cast<double>(vertex.position[axis]); });
			median[axis] = medianOf(values);
		}
		std::ranges::transform(mesh->vertices, values.begin(), [&median](const auto& vertex)
		{
			Vec3 d{vertex.position[0] - median[0], vertex.position[1] - median[1], vertex.position[2] - median[2]};
			return std::sqrt((d[0] * d[0]) + (d[1] * d[1]) + (d[2] * d[2]));
		});
		auto distances = values;
		auto medianDistance = medianOf(values);
		constexpr double kStrayFactor = 100.0; // large ground planes and backdrops reach about 20 (mori_knob)
		auto stray = std::ranges::count_if(distances, [medianDistance](double d) { return d > kStrayFactor * medianDistance; });
		if (medianDistance > 0.0 && stray > 0)
			report.Warn(
				"{} vertices are more than {:.0f} times as far from the median point as the median vertex (stray vertices?)",
				stray, kStrayFactor);
	}

	// stray vertices: far away from the rest, which a broken export leaves (spikes, and bounds, and so a camera framing,
	// far larger than the model). measured from the median point, which they don't drag along as they do the bounds.
	if (!mesh->vertices.empty())
	{
		auto medianOf = [](std::vector<double> values)
		{
			auto middle = values.begin() + static_cast<std::ptrdiff_t>(values.size() / 2);
			std::ranges::nth_element(values, middle);
			return *middle;
		};
		Vec3 median{};
		std::vector<double> values(mesh->vertices.size());
		for (size_t axis = 0; axis < 3; axis++)
		{
			std::ranges::transform(mesh->vertices, values.begin(), [axis](const auto& vertex) { return static_cast<double>(vertex.position[axis]); });
			median[axis] = medianOf(values);
		}
		std::ranges::transform(mesh->vertices, values.begin(), [&median](const auto& vertex)
		{
			Vec3 d{vertex.position[0] - median[0], vertex.position[1] - median[1], vertex.position[2] - median[2]};
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

	auto vertexBytes = mesh->vertices.size() * sizeof(VertexP3fN3fT014fC4f);
	report.Info("vertex buffer {:.1f} MiB, index buffer {:.1f} MiB", vertexBytes / 1048576.0, mesh->indices.size() * 4 / 1048576.0);

	if (stats.droppedTriangles > 0)
		report.Warn("{} triangles dropped (invalid vertex index)", stats.droppedTriangles);
	if (stats.degenerateTriangles > 0)
		report.Info("{} degenerate triangles", stats.degenerateTriangles);
	if (stats.repairedNormals > 0)
		report.Warn("{} unusable normals in the file replaced", stats.repairedNormals);
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
			texturesOut.insert({std::filesystem::weakly_canonical(material.diffuseTexture), gfx::image::Usage::kColor, 1.0F});
		if (!material.alphaTexture.empty())
			texturesOut.insert({std::filesystem::weakly_canonical(material.alphaTexture), gfx::image::Usage::kMask, 1.0F});
		if (!material.occlusionTexture.empty())
			texturesOut.insert({std::filesystem::weakly_canonical(material.occlusionTexture), gfx::image::Usage::kOcclusion, 1.0F});
		if (!material.normalTexture.empty())
			texturesOut.insert({std::filesystem::weakly_canonical(material.normalTexture), gfx::image::Usage::kNormal, 1.0F});
		else if (!material.bumpTexture.empty())
			texturesOut.insert({std::filesystem::weakly_canonical(material.bumpTexture), gfx::image::Usage::kBump, material.bumpScale});
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
	}
	return "?";
}

Report CheckImage(const std::filesystem::path& path, const gfx::image::Options& options)
{
	using gfx::image::Usage;

	Report report;

	static constexpr std::byte kFill{0xcd};
	std::vector<std::byte> data;
	auto image = gfx::image::Import(
		path,
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
	auto reference = gfx::image::Decode(path, options);
	if (!reference)
	{
		report.Fail("reference decode failed: {}", reference.error());
		return report;
	}
	auto width = reference->width;
	auto height = reference->height;
	const auto& pixels = reference->rgba;

	auto format = image->format;
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
				gfx::image::DecompressBlock(
					format, std::span(data).subspan(level.offset + (((by * blockCols) + bx) * blockSize), blockSize), block);
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
		report.Fail("level 0 psnr {:.1f} dB is too low", psnr);
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
			auto sourceAlpha = format == gfx::image::Format::kBC3 ? pixels[(i * 4) + 3] : 255;
			maxAlphaError = std::max(maxAlphaError, static_cast<uint32_t>(std::abs(level0[(i * 4) + 3] - sourceAlpha)));
		}
		if (maxAlphaError > 24)
			report.Fail("max alpha error {} is too high", maxAlphaError);

		if (reference->alpha != (format == gfx::image::Format::kBC3))
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
		report.Fail("mip {} average is off by {:.1f}", worstLevel, worst);
	else if (worst > 12.0)
		report.Warn("mip {} average is off by {:.1f}", worstLevel, worst);
	if (worstSingleBlock > 12.0)
		report.Warn("mip {} (a single block) average is off by {:.1f}", worstSingleBlockLevel, worstSingleBlock);

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
	std::set<ImageCheck> imageFiles;

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
				else if (IsImage(entry.path()))
					imageFiles.insert({std::filesystem::weakly_canonical(entry.path()), gfx::image::Usage::kColor, 1.0F});
			}
		}
		else if (IsModel(path))
		{
			modelFiles.push_back(path);
		}
		else if (IsImage(path))
		{
			imageFiles.insert({std::filesystem::weakly_canonical(path), gfx::image::Usage::kColor, 1.0F});
		}
		else
		{
			std::println(stderr, "not a model, image or directory: {}", arg);
			return 2;
		}
	}

	std::ranges::sort(modelFiles);

	std::map<Result, size_t> modelResults;
	std::map<Result, size_t> encodingResults;
	std::map<Result, size_t> imageResults;

	if (models)
	{
		// by the directory above theirs and their (lower case) name: the encodings of a model
		std::map<std::pair<std::filesystem::path, std::string>, std::vector<std::pair<std::filesystem::path, ModelSummary>>> encodings;

		for (const auto& path : modelFiles)
		{
			auto start = std::chrono::steady_clock::now();
			std::set<ImageCheck> textures;
			std::optional<ModelSummary> summary;
			auto report = CheckModel(path, textures, summary);
			Print(path, report, std::chrono::steady_clock::now() - start);
			modelResults[report.result]++;
			if (images)
				imageFiles.insert(textures.begin(), textures.end());
			if (auto model = path.parent_path().parent_path(); summary && Lower(model.filename().string()) == Lower(path.stem().string()))
				encodings[{model, Lower(path.stem().string())}].emplace_back(path, *summary);
		}

		for (const auto& [key, files] : encodings)
		{
			std::set<std::filesystem::path> directories;
			for (const auto& [path, summary] : files)
				directories.insert(path.parent_path());
			if (files.size() < 2 || directories.size() != files.size())
				continue;

			auto report = CheckEncodings(files);
			Print(key.first / key.second, report, {});
			encodingResults[report.result]++;
		}
	}

	if (images)
	{
		for (const auto& [path, usage, bumpScale] : imageFiles)
		{
			auto start = std::chrono::steady_clock::now();
			auto report = CheckImage(path, {.usage = usage, .bumpScale = bumpScale});
			Print(path, report, std::chrono::steady_clock::now() - start);
			imageResults[report.result]++;
		}
	}

	std::println(
		"models: {} pass, {} warn, {} fail. encodings: {} pass, {} warn. images: {} pass, {} warn, {} fail.",
		modelResults[Result::kPass], modelResults[Result::kWarn], modelResults[Result::kFail],
		encodingResults[Result::kPass], encodingResults[Result::kWarn],
		imageResults[Result::kPass], imageResults[Result::kWarn], imageResults[Result::kFail]);

	return archiveFailed || modelResults[Result::kFail] + imageResults[Result::kFail] > 0 ? 1 : 0;
}
