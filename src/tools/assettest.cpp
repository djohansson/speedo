// imports models and images the way the client does (gfx::obj::Import, gfx::image::Import) and checks the results.
// usage: assettest [--models-only | --images-only] <file or directory>...
// directories are searched recursively for .obj files and images. the textures that models' materials name are
// checked too. prints one line per asset (PASS, WARN or FAIL, with details) and a summary, and exits with 1 if any
// asset failed.

#include <gfx/imageimport.h>
#include <gfx/objimport.h>

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

bool IsModel(const std::filesystem::path& path) { return Lower(path.extension().string()) == ".obj"; }

bool IsImage(const std::filesystem::path& path)
{
	static const std::set<std::string> kExtensions{".png", ".jpg", ".jpeg", ".tga", ".bmp", ".psd", ".gif", ".hdr", ".pic", ".pnm", ".ppm", ".pgm"};
	return kExtensions.contains(Lower(path.extension().string()));
}

using Vec3 = std::array<double, 3>;

Vec3 ToVec3(const float (&v)[3]) { return {v[0], v[1], v[2]}; } //NOLINT(modernize-avoid-c-arrays)

// an image, with the usage it is checked for, and its bump scale for kNormal
using ImageCheck = std::tuple<std::filesystem::path, gfx::image::Usage, float>;

Report CheckModel(const std::filesystem::path& path, std::set<ImageCheck>& texturesOut)
{
	Report report;

	auto mesh = gfx::obj::Import(path);
	if (!mesh)
	{
		report.Fail("{}", mesh.error());
		return report;
	}

	const auto& stats = mesh->stats;

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
		if (!material.bumpTexture.empty())
			texturesOut.insert({std::filesystem::weakly_canonical(material.bumpTexture), gfx::image::Usage::kNormal, material.bumpScale});
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
	case gfx::image::Usage::kMask: return "mask";
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

	if (options.usage == Usage::kNormal)
	{
		// the angle between the reconstructed normals and the reference ones, and how many point below the surface
		double angleSum = 0.0;
		double maxAngle = 0.0;
		for (size_t i = 0; i < pixelCount; i++)
		{
			std::array<double, 3> a{};
			std::array<double, 3> b{};
			for (size_t ch = 0; ch < 3; ch++)
			{
				a[ch] = decodeSigned(level0[(i * 4) + ch]);
				b[ch] = decodeSigned(pixels[(i * 4) + ch]);
			}
			auto dot = (a[0] * b[0]) + (a[1] * b[1]) + (a[2] * b[2]);
			auto lengths = std::sqrt(((a[0] * a[0]) + (a[1] * a[1]) + (a[2] * a[2])) * ((b[0] * b[0]) + (b[1] * b[1]) + (b[2] * b[2])));
			auto angle = std::acos(std::clamp(lengths > 0.0 ? dot / lengths : 1.0, -1.0, 1.0)) * 180.0 / std::numbers::pi;
			angleSum += angle;
			maxAngle = std::max(maxAngle, angle);
		}
		auto meanAngle = angleSum / static_cast<double>(pixelCount);
		report.Info("level 0 normals off by {:.2f} degrees on average, {:.1f} at most", meanAngle, maxAngle);
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

	auto levelAverage0 = average(level0);
	double worst = 0.0;
	size_t worstLevel = 0;
	for (size_t levelIt = 1; levelIt < image->mipLevels.size(); levelIt++)
	{
		auto levelAverage = average(decodeLevel(image->mipLevels[levelIt]));
		for (size_t ch = 0; ch < 4; ch++)
		{
			if (auto d = std::abs(levelAverage[ch] - levelAverage0[ch]); d > worst)
			{
				worst = d;
				worstLevel = levelIt;
			}
		}
	}
	// odd extents drop a row or column per level, so small drifts are expected
	if (worst > 24.0)
		report.Fail("mip {} average is off by {:.1f}", worstLevel, worst);
	else if (worst > 12.0)
		report.Warn("mip {} average is off by {:.1f}", worstLevel, worst);

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
	std::map<Result, size_t> imageResults;

	if (models)
	{
		for (const auto& path : modelFiles)
		{
			auto start = std::chrono::steady_clock::now();
			std::set<ImageCheck> textures;
			auto report = CheckModel(path, textures);
			Print(path, report, std::chrono::steady_clock::now() - start);
			modelResults[report.result]++;
			if (images)
				imageFiles.insert(textures.begin(), textures.end());
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
		"models: {} pass, {} warn, {} fail. images: {} pass, {} warn, {} fail.",
		modelResults[Result::kPass], modelResults[Result::kWarn], modelResults[Result::kFail],
		imageResults[Result::kPass], imageResults[Result::kWarn], imageResults[Result::kFail]);

	return modelResults[Result::kFail] + imageResults[Result::kFail] > 0 ? 1 : 0;
}
