#include "gltfimport.h"

#include <core/profiling.h>
#include <core/utils.h>

#include <algorithm>
#include <cctype>
#include <array>
#include <cmath>
#include <cstring>
#include <format>
#include <fstream>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <system_error>

#define CGLTF_IMPLEMENTATION
#include <cgltf.h>

#include <xxhash.h>

namespace gfx::gltf
{

using mesh::Mesh;
using mesh::Submesh;

namespace detail
{

struct DataDeleter
{
	void operator()(cgltf_data* data) const noexcept { cgltf_free(data); }
};
using DataPtr = std::unique_ptr<cgltf_data, DataDeleter>;

[[nodiscard]] std::string_view ToString(cgltf_result result)
{
	switch (result)
	{
	case cgltf_result_success: return "success";
	case cgltf_result_data_too_short: return "data too short";
	case cgltf_result_unknown_format: return "unknown format";
	case cgltf_result_invalid_json: return "invalid json";
	case cgltf_result_invalid_gltf: return "invalid gltf";
	case cgltf_result_invalid_options: return "invalid options";
	case cgltf_result_file_not_found: return "file not found";
	case cgltf_result_io_error: return "io error";
	case cgltf_result_out_of_memory: return "out of memory";
	case cgltf_result_legacy_gltf: return "legacy (glTF 1.0) file";
	default: return "unknown error";
	}
}

[[nodiscard]] std::expected<DataPtr, std::string> Parse(const std::filesystem::path& path)
{
	cgltf_options options{};
	cgltf_data* data = nullptr;
	if (auto result = cgltf_parse_file(&options, path.string().c_str(), &data); result != cgltf_result_success)
		return std::unexpected(std::format("failed to parse {}: {}", path.string(), ToString(result)));
	return DataPtr(data);
}

// a uri as a relative path: percent decoded (gltf uris are percent encoded)
[[nodiscard]] std::filesystem::path UriPath(const char* uri)
{
	std::string decoded(uri);
	decoded.resize(cgltf_decode_uri(decoded.data()));
	return std::filesystem::path(decoded);
}

[[nodiscard]] bool IsDataUri(const char* uri) { return uri != nullptr && std::string_view(uri).starts_with("data:"); }

// the extensions a file may require: those whose data cgltf reads without help, and those that only add material or
// scene data the renderer ignores
[[nodiscard]] bool IsSupportedRequiredExtension(std::string_view name)
{
	return name == "KHR_mesh_quantization" || name == "KHR_texture_transform" || name.starts_with("KHR_materials_") ||
		   name == "KHR_lights_punctual" || name == "KHR_node_visibility";
}

// whether KHR_node_visibility hides a node (and so its descendants). cgltf leaves the extension as json.
[[nodiscard]] bool IsHidden(const cgltf_node& node)
{
	for (cgltf_size extensionIt = 0; extensionIt < node.extensions_count; extensionIt++)
	{
		const auto& extension = node.extensions[extensionIt];
		if (extension.name == nullptr || extension.data == nullptr || std::string_view(extension.name) != "KHR_node_visibility")
			continue;

		std::string json(extension.data);
		std::erase_if(json, [](unsigned char c) { return std::isspace(c) != 0; });
		return json.contains("\"visible\":false");
	}
	return false;
}

using Vec3 = std::array<double, 3>;

[[nodiscard]] Vec3 Sub(const Vec3& a, const Vec3& b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
[[nodiscard]] Vec3 Cross(const Vec3& a, const Vec3& b)
{
	return {(a[1] * b[2]) - (a[2] * b[1]), (a[2] * b[0]) - (a[0] * b[2]), (a[0] * b[1]) - (a[1] * b[0])};
}
[[nodiscard]] double Dot(const Vec3& a, const Vec3& b) { return (a[0] * b[0]) + (a[1] * b[1]) + (a[2] * b[2]); }
[[nodiscard]] double Length(const Vec3& a) { return std::sqrt(Dot(a, a)); }
[[nodiscard]] bool IsFinite(const Vec3& a) { return std::isfinite(a[0]) && std::isfinite(a[1]) && std::isfinite(a[2]); }

// a column major 4x4 matrix, as gltf stores them
using Matrix = std::array<float, 16>;

[[nodiscard]] Vec3 TransformPoint(const Matrix& m, const Vec3& p)
{
	return {
		(m[0] * p[0]) + (m[4] * p[1]) + (m[8] * p[2]) + m[12],
		(m[1] * p[0]) + (m[5] * p[1]) + (m[9] * p[2]) + m[13],
		(m[2] * p[0]) + (m[6] * p[1]) + (m[10] * p[2]) + m[14]};
}

// the inverse transpose of the upper 3x3 scaled by the determinant (which keeps it finite for singular matrices): its
// cofactor matrix. a negative determinant (a mirroring transform) flips the normals it transforms, see TransformNormal.
[[nodiscard]] std::array<double, 9> NormalMatrix(const Matrix& m)
{
	auto a = [&m](int row, int col) { return static_cast<double>(m[(col * 4) + row]); };
	return {
		(a(1, 1) * a(2, 2)) - (a(1, 2) * a(2, 1)), (a(1, 2) * a(2, 0)) - (a(1, 0) * a(2, 2)), (a(1, 0) * a(2, 1)) - (a(1, 1) * a(2, 0)),
		(a(0, 2) * a(2, 1)) - (a(0, 1) * a(2, 2)), (a(0, 0) * a(2, 2)) - (a(0, 2) * a(2, 0)), (a(0, 1) * a(2, 0)) - (a(0, 0) * a(2, 1)),
		(a(0, 1) * a(1, 2)) - (a(0, 2) * a(1, 1)), (a(0, 2) * a(1, 0)) - (a(0, 0) * a(1, 2)), (a(0, 0) * a(1, 1)) - (a(0, 1) * a(1, 0))};
}

[[nodiscard]] double Determinant(const Matrix& m)
{
	auto a = [&m](int row, int col) { return static_cast<double>(m[(col * 4) + row]); };
	return (a(0, 0) * ((a(1, 1) * a(2, 2)) - (a(1, 2) * a(2, 1)))) - (a(0, 1) * ((a(1, 0) * a(2, 2)) - (a(1, 2) * a(2, 0)))) +
		   (a(0, 2) * ((a(1, 0) * a(2, 1)) - (a(1, 1) * a(2, 0))));
}

[[nodiscard]] Vec3 TransformNormal(const std::array<double, 9>& n, const Vec3& v)
{
	// rows of the cofactor matrix
	return {
		(n[0] * v[0]) + (n[1] * v[1]) + (n[2] * v[2]),
		(n[3] * v[0]) + (n[4] * v[1]) + (n[5] * v[2]),
		(n[6] * v[0]) + (n[7] * v[1]) + (n[8] * v[2])};
}

// how a material's vertices take their texcoords: which set goes first (the one its textures sample), and the base
// color texture's transform, if any
struct TexCoordMapping
{
	int set = 0;
	std::optional<cgltf_texture_transform> transform;
};

[[nodiscard]] std::array<float, 2> Transform(const cgltf_texture_transform& t, float u, float v)
{
	// offset * rotation * scale, with the rotation as KHR_texture_transform defines it (counter-clockwise in uv space,
	// whose v points down)
	auto su = static_cast<double>(u) * t.scale[0];
	auto sv = static_cast<double>(v) * t.scale[1];
	auto c = std::cos(static_cast<double>(t.rotation));
	auto s = std::sin(static_cast<double>(t.rotation));
	return {static_cast<float>((c * su) + (s * sv) + t.offset[0]), static_cast<float>((-s * su) + (c * sv) + t.offset[1])};
}

// the image files of a file's textures: external ones are resolved next to the file, embedded ones (in a buffer view,
// or a data uri) are written to the embedded image directory, once per image
class Images
{
public:
	Images(const cgltf_data& data, const std::filesystem::path& path, const mesh::ImportOptions& options, mesh::Stats& stats)
		: myData(data)
		, myBaseDir(path.parent_path())
		, myStem(path.stem().string())
		, myEmbeddedDir(options.embeddedImageDirectory)
		, myStats(stats)
		, myPaths(data.images_count)
	{}

	// the file of a texture, or empty (counted as missing) if it has none that can be loaded
	[[nodiscard]] std::filesystem::path Resolve(const cgltf_texture_view& view, std::string_view material)
	{
		const auto* texture = view.texture;
		if (texture == nullptr)
			return {};

		if (texture->image == nullptr)
		{
			const char* extension = texture->has_basisu ? "KHR_texture_basisu" : texture->has_webp ? "EXT_texture_webp" : nullptr;
			Missing(material, extension != nullptr ? std::format("the texture is only in a format of {}", extension) : "the texture has no image");
			return {};
		}

		auto index = static_cast<size_t>(texture->image - myData.images);
		if (!myPaths[index])
			myPaths[index] = InternalResolve(*texture->image, index);
		if (myPaths[index]->empty())
			Missing(material, myErrors[index]);
		return *myPaths[index];
	}

private:
	void Missing(std::string_view material, std::string_view why)
	{
		myStats.missingTextures++;
		myStats.warnings.emplace_back(std::format("material {}: texture not loaded: {}", material, why));
	}

	[[nodiscard]] static std::string_view ExtensionOf(std::string_view mimeType)
	{
		if (mimeType == "image/png")
			return ".png";
		if (mimeType == "image/jpeg")
			return ".jpg";
		return {};
	}

	[[nodiscard]] std::filesystem::path InternalResolve(const cgltf_image& image, size_t index)
	{
		auto& error = myErrors[index];

		if (image.uri != nullptr && !IsDataUri(image.uri))
		{
			auto path = myBaseDir / UriPath(image.uri);
			auto extension = path.extension().string();
			std::ranges::transform(extension, extension.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			if (extension == ".ktx2" || extension == ".webp")
			{
				error = std::format("{}: {} images aren't supported", path.filename().string(), extension);
				return {};
			}
			if (std::error_code ec; !std::filesystem::is_regular_file(path, ec))
			{
				error = std::format("{} not found", path.string());
				return {};
			}
			return path;
		}

		// embedded: in a buffer view (with a mime type), or a base64 data uri (data:<mime type>;base64,<data>)
		std::string_view mimeType = image.mime_type != nullptr ? image.mime_type : "";
		std::span<const std::byte> bytes;
		std::unique_ptr<void, void (*)(void*)> decoded(nullptr, std::free);
		if (image.buffer_view != nullptr)
		{
			const auto* data = reinterpret_cast<const std::byte*>(cgltf_buffer_view_data(image.buffer_view));
			if (data == nullptr)
			{
				error = "its buffer isn't loaded";
				return {};
			}
			bytes = std::span(data, image.buffer_view->size);
		}
		else if (IsDataUri(image.uri))
		{
			std::string_view uri(image.uri);
			auto comma = uri.find(',');
			auto header = uri.substr(0, comma);
			if (comma == std::string_view::npos || !header.ends_with(";base64"))
			{
				error = "a data uri that isn't base64";
				return {};
			}
			mimeType = header.substr(5, header.size() - 5 - 7); // between "data:" and ";base64"
			auto base64 = uri.substr(comma + 1);
			auto padding = std::ranges::count(base64.substr(base64.size() >= 2 ? base64.size() - 2 : 0), '=');
			auto size = ((base64.size() / 4) * 3) - static_cast<size_t>(padding);
			cgltf_options options{};
			void* out = nullptr;
			if (cgltf_load_buffer_base64(&options, size, base64.data(), &out) != cgltf_result_success)
			{
				error = "its data uri doesn't decode";
				return {};
			}
			decoded.reset(out);
			bytes = std::span(static_cast<const std::byte*>(out), size);
		}
		else
		{
			error = "it has neither a uri nor a buffer view";
			return {};
		}

		auto extension = ExtensionOf(mimeType);
		if (extension.empty())
		{
			error = std::format("embedded {} images aren't supported", mimeType.empty() ? "untyped" : mimeType);
			return {};
		}

		if (myEmbeddedDir.empty())
		{
			error = "embedded, and there is no directory to extract it to";
			return {};
		}

		// named after the content, so that unchanged images are written once
		auto path = myEmbeddedDir /
					std::format("{}-{}-{:016x}{}", myStem, index, XXH3_64bits(bytes.data(), bytes.size()), extension);
		std::error_code ec;
		if (std::filesystem::is_regular_file(path, ec))
			return path;

		std::filesystem::create_directories(myEmbeddedDir, ec);
		auto temporary = path;
		temporary += ".tmp";
		{
			std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
			file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
			if (!file)
			{
				error = std::format("failed to write {}", temporary.string());
				return {};
			}
		}
		std::filesystem::rename(temporary, path, ec);
		if (ec)
		{
			error = std::format("failed to write {}: {}", path.string(), ec.message());
			return {};
		}
		return path;
	}

	const cgltf_data& myData;
	std::filesystem::path myBaseDir;
	std::string myStem;
	std::filesystem::path myEmbeddedDir;
	mesh::Stats& myStats;
	std::vector<std::optional<std::filesystem::path>> myPaths; // by image index, once resolved
	core::UnorderedMap<size_t, std::string> myErrors; // why an image resolved to nothing
};

// an unsigned integer of a component type an index can have
[[nodiscard]] std::optional<uint32_t> ReadIndexComponent(const uint8_t* data, cgltf_component_type type)
{
	switch (type)
	{
	case cgltf_component_type_r_8u: return *data;
	case cgltf_component_type_r_16u:
	{
		uint16_t value = 0;
		std::memcpy(&value, data, sizeof(value));
		return value;
	}
	case cgltf_component_type_r_32u:
	{
		uint32_t value = 0;
		std::memcpy(&value, data, sizeof(value));
		return value;
	}
	default: return std::nullopt;
	}
}

// reads an index accessor, sparse ones too (cgltf_accessor_unpack_indices doesn't apply them): the base values (zeros
// without a buffer view), then the sparse ones replacing them. read as integers, which the float path would round
// above 2^24.
[[nodiscard]] bool ReadIndices(const cgltf_accessor& accessor, std::span<uint32_t> out)
{
	if (!accessor.is_sparse)
		return cgltf_accessor_unpack_indices(&accessor, out.data(), sizeof(uint32_t), out.size()) == out.size();

	// cgltf_accessor_read_index refuses sparse accessors (it returns 0), so the base values are read here
	if (accessor.buffer_view != nullptr)
	{
		const auto* base = static_cast<const uint8_t*>(cgltf_buffer_view_data(accessor.buffer_view));
		if (base == nullptr)
			return false;
		base += accessor.offset;
		for (size_t i = 0; i < out.size(); i++)
		{
			auto value = ReadIndexComponent(base + (i * accessor.stride), accessor.component_type);
			if (!value)
				return false;
			out[i] = *value;
		}
	}
	else
	{
		std::ranges::fill(out, 0U);
	}

	const auto& sparse = accessor.sparse;
	const auto* indices = static_cast<const uint8_t*>(cgltf_buffer_view_data(sparse.indices_buffer_view));
	const auto* values = static_cast<const uint8_t*>(cgltf_buffer_view_data(sparse.values_buffer_view));
	if (indices == nullptr || values == nullptr)
		return false;
	indices += sparse.indices_byte_offset;
	values += sparse.values_byte_offset;

	auto indexSize = cgltf_component_size(sparse.indices_component_type);
	auto valueSize = cgltf_component_size(accessor.component_type);
	for (size_t i = 0; i < sparse.count; i++)
	{
		auto element = ReadIndexComponent(indices + (i * indexSize), sparse.indices_component_type);
		auto value = ReadIndexComponent(values + (i * valueSize), accessor.component_type);
		if (!element || !value || *element >= out.size())
			return false;
		out[*element] = *value;
	}
	return true;
}

// a primitive's triangles, before they are put in the mesh's material order
struct Part
{
	int32_t material = -1;
	std::vector<VertexP3fN3fT014fC4f> vertices;
	std::vector<uint32_t> indices;
};

} // namespace detail

std::expected<Mesh, std::string> Import(
	const std::filesystem::path& path, const mesh::ImportOptions& options, const std::function<bool()>& cancelled)
{
	using namespace detail;

	ZoneScopedN("gltf::Import");

	auto isCancelled = [&cancelled] { return cancelled && cancelled(); };

	auto parsed = Parse(path);
	if (!parsed)
		return std::unexpected(parsed.error());
	auto& data = **parsed;

	for (cgltf_size extensionIt = 0; extensionIt < data.extensions_required_count; extensionIt++)
		if (std::string_view extension = data.extensions_required[extensionIt]; !IsSupportedRequiredExtension(extension))
			return std::unexpected(std::format("{} requires {}, which isn't supported", path.string(), extension));

	{
		ZoneScopedN("gltf::Import::buffers");

		cgltf_options loadOptions{};
		if (auto result = cgltf_load_buffers(&loadOptions, &data, path.string().c_str()); result != cgltf_result_success)
			return std::unexpected(std::format("failed to load the buffers of {}: {}", path.string(), ToString(result)));
	}

	if (auto result = cgltf_validate(&data); result != cgltf_result_success)
		return std::unexpected(std::format("{} is invalid: {}", path.string(), ToString(result)));

	if (isCancelled())
		return std::unexpected("cancelled");

	Mesh mesh;
	auto& stats = mesh.stats;
	auto warn = [&stats]<typename... Args>(std::format_string<Args...> fmt, Args&&... args)
	{ stats.warnings.push_back(std::format(fmt, std::forward<Args>(args)...)); };

	// materials, and how their vertices take texcoords
	Images images(data, path, options, stats);
	std::vector<TexCoordMapping> texCoordMappings(data.materials_count);
	std::vector<bool> doubleSided(data.materials_count);
	std::vector<std::array<float, 4>> baseColorFactors(data.materials_count);
	bool blendWarned = false;
	mesh.materials.reserve(data.materials_count);
	for (cgltf_size materialIt = 0; materialIt < data.materials_count; materialIt++)
	{
		const auto& gltfMaterial = data.materials[materialIt];
		auto& material = mesh.materials.emplace_back();
		material.name = gltfMaterial.name != nullptr ? gltfMaterial.name : std::format("material {}", materialIt);

		// metallic-roughness by default; the (archived) specular-glossiness extension's diffuse otherwise
		const cgltf_texture_view* baseColorTexture = &gltfMaterial.pbr_metallic_roughness.base_color_texture;
		const cgltf_float* baseColorFactor = gltfMaterial.pbr_metallic_roughness.base_color_factor;
		if (!gltfMaterial.has_pbr_metallic_roughness && gltfMaterial.has_pbr_specular_glossiness)
		{
			baseColorTexture = &gltfMaterial.pbr_specular_glossiness.diffuse_texture;
			baseColorFactor = gltfMaterial.pbr_specular_glossiness.diffuse_factor;
		}
		std::array<float, 4> factor{1.0F, 1.0F, 1.0F, 1.0F};
		if (gltfMaterial.has_pbr_metallic_roughness || gltfMaterial.has_pbr_specular_glossiness)
			std::copy_n(baseColorFactor, 4, factor.begin());
		baseColorFactors[materialIt] = factor;
		std::copy_n(factor.begin(), 3, material.diffuse.begin());
		material.dissolve = factor[3];

		material.diffuseTexture = images.Resolve(*baseColorTexture, material.name);
		material.normalTexture = images.Resolve(gltfMaterial.normal_texture, material.name);
		material.normalScale = gltfMaterial.normal_texture.scale;
		auto emissiveStrength = gltfMaterial.has_emissive_strength ? gltfMaterial.emissive_strength.emissive_strength : 1.0F;
		for (size_t channel = 0; channel < 3; channel++)
			material.emissive[channel] = gltfMaterial.emissive_factor[channel] * emissiveStrength;
		material.emissiveTexture = images.Resolve(gltfMaterial.emissive_texture, material.name);
		material.occlusionTexture = images.Resolve(gltfMaterial.occlusion_texture, material.name);
		material.occlusionStrength = gltfMaterial.occlusion_texture.scale; // cgltf keeps the strength as scale

		switch (gltfMaterial.alpha_mode)
		{
		case cgltf_alpha_mode_opaque: material.alphaCutoff = 0.0F; break;
		case cgltf_alpha_mode_mask: material.alphaCutoff = gltfMaterial.alpha_cutoff; break;
		case cgltf_alpha_mode_blend:
			material.alphaCutoff = 0.5F;
			if (!std::exchange(blendWarned, true))
				warn("alpha blended materials (e.g. {}) are drawn alpha tested", material.name);
			break;
		default: break;
		}

		doubleSided[materialIt] = gltfMaterial.double_sided != 0;

		// the textures this renderer samples share one texcoord set: the base color texture's (or the normal map's)
		auto setOf = [](const cgltf_texture_view& view) { return view.has_transform && view.transform.has_texcoord ? view.transform.texcoord : view.texcoord; };
		const auto* primary = baseColorTexture->texture != nullptr ? baseColorTexture : &gltfMaterial.normal_texture;
		auto& mapping = texCoordMappings[materialIt];
		mapping.set = setOf(*primary);
		if (primary->has_transform)
			mapping.transform = primary->transform;
		if (baseColorTexture->texture != nullptr && gltfMaterial.normal_texture.texture != nullptr &&
			(setOf(gltfMaterial.normal_texture) != mapping.set ||
			 gltfMaterial.normal_texture.has_transform != baseColorTexture->has_transform))
			warn("material {}: the normal map's texcoords (set or transform) differ from the base color's, and are ignored", material.name);
		if (mapping.set > 1)
		{
			warn("material {}: texcoord set {} isn't supported, set 0 is used", material.name, mapping.set);
			mapping.set = 0;
		}
	}

	auto materialOf = [&data](const cgltf_primitive& primitive)
	{ return primitive.material != nullptr ? static_cast<int32_t>(primitive.material - data.materials) : -1; };

	// the primitives of the scene's nodes, in their world space
	std::vector<Part> parts;
	size_t skippedPrimitives = 0;
	bool instancingWarned = false;
	bool skinWarned = false;
	bool morphWarned = false; // about weights that can't be applied
	double agreeingArea = 0.0;
	double normalArea = 0.0;

	// weights: the morph target weights (the node's, else the mesh's defaults), see the deltas below
	auto addPrimitive = [&](const cgltf_primitive& primitive, const Matrix& world, std::span<const cgltf_float> weights, std::string_view meshName) -> void
	{
		if (primitive.type != cgltf_primitive_type_triangles && primitive.type != cgltf_primitive_type_triangle_strip &&
			primitive.type != cgltf_primitive_type_triangle_fan)
		{
			skippedPrimitives++;
			return;
		}


		const cgltf_accessor* positions = nullptr;
		const cgltf_accessor* normals = nullptr;
		std::array<const cgltf_accessor*, 2> texCoords{};
		const cgltf_accessor* colors = nullptr;
		for (cgltf_size attributeIt = 0; attributeIt < primitive.attributes_count; attributeIt++)
		{
			const auto& attribute = primitive.attributes[attributeIt];
			switch (attribute.type)
			{
			case cgltf_attribute_type_position:
				if (attribute.index == 0)
					positions = attribute.data;
				break;
			case cgltf_attribute_type_normal:
				if (attribute.index == 0)
					normals = attribute.data;
				break;
			case cgltf_attribute_type_texcoord:
				if (attribute.index >= 0 && attribute.index < 2)
					texCoords[attribute.index] = attribute.data;
				break;
			case cgltf_attribute_type_color:
				if (attribute.index == 0)
					colors = attribute.data;
				break;
			default: break;
			}
		}

		if (positions == nullptr || positions->type != cgltf_type_vec3)
		{
			warn("mesh {}: a primitive without positions is skipped", meshName);
			skippedPrimitives++;
			return;
		}

		const auto vertexCount = positions->count;
		auto unpack = [vertexCount](const cgltf_accessor* accessor, size_t components) -> std::optional<std::vector<float>>
		{
			std::vector<float> values(vertexCount * components);
			if (accessor == nullptr || accessor->count != vertexCount ||
				cgltf_accessor_unpack_floats(accessor, values.data(), values.size()) != values.size())
				return std::nullopt;
			return values;
		};

		auto positionValues = unpack(positions, 3);
		if (!positionValues)
		{
			// e.g. draco or meshopt compressed, without a fallback
			warn("mesh {}: a primitive whose positions can't be read is skipped", meshName);
			skippedPrimitives++;
			return;
		}
		auto normalValues = normals != nullptr && normals->type == cgltf_type_vec3 ? unpack(normals, 3) : std::nullopt;

		// morph targets, at their weights (animating them belongs to animation, which is ignored): each target's
		// position and normal deltas, times its weight, are added to the base mesh. tangents are ignored anyway.
		for (cgltf_size targetIt = 0; targetIt < primitive.targets_count; targetIt++)
		{
			auto weight = targetIt < weights.size() ? weights[targetIt] : 0.0F;
			if (weight == 0.0F)
				continue;

			const auto& target = primitive.targets[targetIt];
			for (cgltf_size attributeIt = 0; attributeIt < target.attributes_count; attributeIt++)
			{
				const auto& attribute = target.attributes[attributeIt];
				auto* values = attribute.type == cgltf_attribute_type_position ? &positionValues
							   : attribute.type == cgltf_attribute_type_normal ? &normalValues
																				 : nullptr;
				if (values == nullptr || !*values || attribute.data->type != cgltf_type_vec3)
					continue;

				auto deltas = unpack(attribute.data, 3);
				if (!deltas)
				{
					if (!std::exchange(morphWarned, true))
						warn("mesh {}: a morph target can't be read, and is ignored", meshName);
					continue;
				}
				for (size_t i = 0; i < deltas->size(); i++)
					(**values)[i] += weight * (*deltas)[i];
			}
		}
		std::array<std::optional<std::vector<float>>, 2> texCoordValues;
		for (size_t set = 0; set < texCoords.size(); set++)
			if (texCoords[set] != nullptr && texCoords[set]->type == cgltf_type_vec2)
				texCoordValues[set] = unpack(texCoords[set], 2);
		size_t colorComponents = colors != nullptr && colors->type == cgltf_type_vec4 ? 4 : 3;
		auto colorValues = colors != nullptr && (colors->type == cgltf_type_vec3 || colors->type == cgltf_type_vec4)
							   ? unpack(colors, colorComponents)
							   : std::nullopt;

		mesh.hasNormals |= normalValues.has_value();
		mesh.hasTexCoords |= texCoordValues[0].has_value() || texCoordValues[1].has_value();
		mesh.hasColors |= colorValues.has_value();

		// the triangle list, in the primitive's vertices
		std::vector<uint32_t> elements;
		if (primitive.indices != nullptr)
		{
			elements.resize(primitive.indices->count);
			if (!ReadIndices(*primitive.indices, elements))
			{
				warn("mesh {}: a primitive whose indices can't be read is skipped", meshName);
				skippedPrimitives++;
				return;
			}
		}
		else
		{
			elements.resize(vertexCount);
			for (uint32_t i = 0; i < vertexCount; i++)
				elements[i] = i;
		}

		std::vector<std::array<uint32_t, 3>> triangles;
		switch (primitive.type)
		{
		case cgltf_primitive_type_triangles:
			for (size_t i = 0; i + 2 < elements.size(); i += 3)
				triangles.push_back({elements[i], elements[i + 1], elements[i + 2]});
			break;
		case cgltf_primitive_type_triangle_strip:
			for (size_t i = 0; i + 2 < elements.size(); i++)
				triangles.push_back(i % 2 == 0 ? std::array{elements[i], elements[i + 1], elements[i + 2]}
											   : std::array{elements[i + 1], elements[i], elements[i + 2]});
			break;
		case cgltf_primitive_type_triangle_fan:
			for (size_t i = 1; i + 1 < elements.size(); i++)
				triangles.push_back({elements[0], elements[i], elements[i + 1]});
			break;
		default: break;
		}

		auto material = materialOf(primitive);
		const auto* mapping = material >= 0 ? &texCoordMappings[material] : nullptr;
		auto factor = material >= 0 ? baseColorFactors[material] : std::array<float, 4>{1.0F, 1.0F, 1.0F, 1.0F};

		bool mirrored = Determinant(world) < 0.0;
		auto normalMatrix = NormalMatrix(world);
		if (mirrored)
			std::ranges::transform(normalMatrix, normalMatrix.begin(), [](double v) { return -v; });

		auto& part = parts.emplace_back(Part{.material = material});

		// the vertices in world space, before normals are generated or repaired
		std::vector<VertexP3fN3fT014fC4f> vertices(vertexCount);
		std::vector<bool> normalValid(vertexCount, false);
		for (size_t vertexIt = 0; vertexIt < vertexCount; vertexIt++)
		{
			auto& vertex = vertices[vertexIt];

			Vec3 p{(*positionValues)[3 * vertexIt], (*positionValues)[(3 * vertexIt) + 1], (*positionValues)[(3 * vertexIt) + 2]};
			p = TransformPoint(world, p);
			if (!IsFinite(p))
			{
				stats.nonFiniteValues++;
				p = {};
			}
			std::ranges::transform(p, vertex.position, [](double v) { return static_cast<float>(v); });

			if (normalValues)
			{
				Vec3 n{(*normalValues)[3 * vertexIt], (*normalValues)[(3 * vertexIt) + 1], (*normalValues)[(3 * vertexIt) + 2]};
				n = TransformNormal(normalMatrix, n);
				if (auto length = Length(n); IsFinite(n) && length > 0.0)
				{
					normalValid[vertexIt] = true;
					for (size_t i = 0; i < 3; i++)
						vertex.normal[i] = static_cast<float>(n[i] / length);
				}
			}

			for (size_t set = 0; set < texCoordValues.size(); set++)
			{
				// the set the material's textures sample goes first
				auto slot = mapping != nullptr && mapping->set == 1 ? 1 - set : set;
				if (!texCoordValues[set])
					continue;
				auto u = (*texCoordValues[set])[2 * vertexIt];
				auto v = (*texCoordValues[set])[(2 * vertexIt) + 1];
				if (!std::isfinite(u) || !std::isfinite(v))
				{
					stats.nonFiniteValues++;
					u = v = 0.0F;
				}
				if (slot == 0 && mapping != nullptr && mapping->transform)
				{
					auto [tu, tv] = Transform(*mapping->transform, u, v);
					u = tu;
					v = tv;
				}
				vertex.texCoord01[2 * slot] = u;
				vertex.texCoord01[(2 * slot) + 1] = v;
			}

			std::array<float, 4> color{1.0F, 1.0F, 1.0F, 1.0F};
			if (colorValues)
				for (size_t i = 0; i < colorComponents; i++)
					color[i] = (*colorValues)[(colorComponents * vertexIt) + i];
			for (size_t i = 0; i < 4; i++)
				vertex.color[i] = color[i] * factor[i];
		}

		auto faceNormalOf = [&vertices](const std::array<uint32_t, 3>& t)
		{
			auto position = [&vertices](uint32_t i) { return Vec3{vertices[i].position[0], vertices[i].position[1], vertices[i].position[2]}; };
			return Cross(Sub(position(t[1]), position(t[0])), Sub(position(t[2]), position(t[0])));
		};

		std::vector<bool> repaired(vertexCount, false);
		for (auto& triangle : triangles)
		{
			if (std::ranges::any_of(triangle, [vertexCount](uint32_t i) { return i >= vertexCount; }))
			{
				stats.droppedTriangles++;
				continue;
			}
			// a mirroring transform turns counter-clockwise triangles clockwise
			if (mirrored)
				std::swap(triangle[1], triangle[2]);

			stats.triangleCount++;
			auto faceNormal = faceNormalOf(triangle);
			auto area = Length(faceNormal);
			bool degenerate = !std::isfinite(area) || area == 0.0;
			if (degenerate)
				stats.degenerateTriangles++;

			if (!normalValues)
			{
				// flat: three vertices of its own
				Vec3 n = degenerate ? Vec3{0.0, 1.0, 0.0} : Vec3{faceNormal[0] / area, faceNormal[1] / area, faceNormal[2] / area};
				for (auto corner : triangle)
				{
					auto vertex = vertices[corner];
					std::ranges::transform(n, vertex.normal, [](double v) { return static_cast<float>(v); });
					part.indices.push_back(static_cast<uint32_t>(part.vertices.size()));
					part.vertices.push_back(vertex);
					stats.generatedNormals++;
				}
				continue;
			}

			// unusable normals in the file take the normal of the first face around them
			for (auto corner : triangle)
			{
				if (normalValid[corner] || repaired[corner])
					continue;
				repaired[corner] = true;
				stats.repairedNormals++;
				Vec3 n = degenerate ? Vec3{0.0, 1.0, 0.0} : Vec3{faceNormal[0] / area, faceNormal[1] / area, faceNormal[2] / area};
				std::ranges::transform(n, vertices[corner].normal, [](double v) { return static_cast<float>(v); });
			}

			if (!degenerate)
			{
				Vec3 vertexNormals{};
				for (auto corner : triangle)
					for (size_t i = 0; i < 3; i++)
						vertexNormals[i] += vertices[corner].normal[i];
				normalArea += area;
				if (Dot(faceNormal, vertexNormals) > 0.0)
					agreeingArea += area;
			}

			for (auto corner : triangle)
				part.indices.push_back(corner);
		}
		if (normalValues)
			part.vertices = std::move(vertices);

		// the renderer culls back faces: a double sided material's back faces are triangles of their own
		if (material >= 0 && doubleSided[material])
		{
			auto vertexOffset = static_cast<uint32_t>(part.vertices.size());
			auto indexCount = part.indices.size();
			part.vertices.reserve(2 * part.vertices.size());
			for (size_t vertexIt = 0; vertexIt < vertexOffset; vertexIt++)
			{
				auto vertex = part.vertices[vertexIt];
				for (auto& n : vertex.normal)
					n = -n;
				part.vertices.push_back(vertex);
			}
			for (size_t i = 0; i + 2 < indexCount; i += 3)
			{
				part.indices.push_back(vertexOffset + part.indices[i]);
				part.indices.push_back(vertexOffset + part.indices[i + 2]);
				part.indices.push_back(vertexOffset + part.indices[i + 1]);
			}
		}
	};

	// the default scene (or the first), or all root nodes if there are no scenes
	std::vector<const cgltf_node*> roots;
	if (const auto* scene = data.scene != nullptr ? data.scene : data.scenes_count > 0 ? &data.scenes[0] : nullptr)
		for (cgltf_size nodeIt = 0; nodeIt < scene->nodes_count; nodeIt++)
			roots.push_back(scene->nodes[nodeIt]);
	else
		for (cgltf_size nodeIt = 0; nodeIt < data.nodes_count; nodeIt++)
			if (data.nodes[nodeIt].parent == nullptr)
				roots.push_back(&data.nodes[nodeIt]);
	if (data.scenes_count > 1)
		warn("{} scenes, only {} is loaded", data.scenes_count, data.scene != nullptr ? "the default one" : "the first");

	{
		ZoneScopedN("gltf::Import::nodes");

		std::vector<const cgltf_node*> stack(roots.rbegin(), roots.rend());
		while (!stack.empty())
		{
			const auto* node = stack.back();
			stack.pop_back();
			if (IsHidden(*node))
				continue;
			for (auto childIt = node->children_count; childIt > 0; childIt--)
				stack.push_back(node->children[childIt - 1]);

			if (node->mesh == nullptr)
				continue;

			if (node->has_mesh_gpu_instancing && !std::exchange(instancingWarned, true))
				warn("instanced meshes (EXT_mesh_gpu_instancing) are drawn once");

			// a skinned mesh is placed by its joints, not its node: without skinning, it is drawn in its bind pose
			Matrix world{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
			if (node->skin != nullptr)
			{
				if (!std::exchange(skinWarned, true))
					warn("skinned meshes are drawn in their bind pose");
			}
			else
			{
				cgltf_node_transform_world(node, world.data());
			}

			// a node's morph target weights override its mesh's defaults
			auto weights = node->weights_count > 0 ? std::span<const cgltf_float>(node->weights, node->weights_count)
												   : std::span<const cgltf_float>(node->mesh->weights, node->mesh->weights_count);

			std::string meshName = node->mesh->name != nullptr ? node->mesh->name : std::format("{}", node->mesh - data.meshes);
			for (cgltf_size primitiveIt = 0; primitiveIt < node->mesh->primitives_count; primitiveIt++)
				addPrimitive(node->mesh->primitives[primitiveIt], world, weights, meshName);

			if (isCancelled())
				return std::unexpected("cancelled");
		}
	}

	if (skippedPrimitives > 0)
		warn("{} primitives skipped (points, lines, or unreadable data)", skippedPrimitives);
	if (data.animations_count > 0)
		warn("{} animations are ignored", data.animations_count);
	if (normalArea > 0.0)
		stats.windingAgreement = agreeingArea / normalArea;

	// the parts in material order (-1 last), one submesh per material
	{
		ZoneScopedN("gltf::Import::merge");

		size_t vertexTotal = 0;
		size_t indexTotal = 0;
		for (const auto& part : parts)
		{
			vertexTotal += part.vertices.size();
			indexTotal += part.indices.size();
		}
		mesh.vertices.reserve(vertexTotal);
		mesh.indices.reserve(indexTotal);

		auto bucketOf = [&mesh](int32_t material) { return material >= 0 ? static_cast<size_t>(material) : mesh.materials.size(); };
		std::ranges::stable_sort(parts, {}, [&bucketOf](const Part& part) { return bucketOf(part.material); });

		bool firstVertex = true;
		for (auto& part : parts)
		{
			if (part.indices.empty())
				continue;

			if (mesh.submeshes.empty() || mesh.submeshes.back().material != part.material)
				mesh.submeshes.push_back(Submesh{
					.firstIndex = static_cast<uint32_t>(mesh.indices.size()), .indexCount = 0, .material = part.material});

			auto vertexOffset = static_cast<uint32_t>(mesh.vertices.size());
			for (auto index : part.indices)
				mesh.indices.push_back(vertexOffset + index);
			mesh.submeshes.back().indexCount += static_cast<uint32_t>(part.indices.size());

			for (const auto& vertex : part.vertices)
			{
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
			}
			mesh.vertices.insert(mesh.vertices.end(), part.vertices.begin(), part.vertices.end());

			std::vector<VertexP3fN3fT014fC4f>().swap(part.vertices);
			std::vector<uint32_t>().swap(part.indices);
		}
	}

	return mesh;
}

std::optional<std::string> UnsupportedRequiredExtension(const std::filesystem::path& path)
{
	auto parsed = detail::Parse(path);
	if (!parsed)
		return std::nullopt; // Import reports it

	const auto& data = **parsed;
	for (cgltf_size extensionIt = 0; extensionIt < data.extensions_required_count; extensionIt++)
		if (std::string_view extension = data.extensions_required[extensionIt]; !detail::IsSupportedRequiredExtension(extension))
			return std::string(extension);
	return std::nullopt;
}

std::vector<std::filesystem::path> BufferFiles(const std::filesystem::path& path)
{
	std::vector<std::filesystem::path> files;

	auto parsed = detail::Parse(path);
	if (!parsed)
		return files;

	const auto& data = **parsed;
	for (cgltf_size bufferIt = 0; bufferIt < data.buffers_count; bufferIt++)
		if (const char* uri = data.buffers[bufferIt].uri; uri != nullptr && !detail::IsDataUri(uri))
			files.push_back(path.parent_path() / detail::UriPath(uri));

	std::ranges::sort(files);
	files.erase(std::ranges::unique(files).begin(), files.end());

	return files;
}

} // namespace gfx::gltf
