#include "gltfimport.h"
#include "tangents.h"

#include <core/profiling.h>
#include <core/utils.h>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <array>
#include <cmath>
#include <cstring>
#include <format>
#include <fstream>
#include <memory>
#include <numeric>
#include <optional>
#include <ranges>
#include <span>
#include <string_view>
#include <system_error>
#include <tuple>

#define CGLTF_IMPLEMENTATION
#include <cgltf.h>
#include <draco/compression/decode.h>
#include <meshoptimizer.h>

#include <xxhash.h>

namespace gfx::gltf
{

using mesh::Mesh;
using mesh::Submesh;
using gfx::SceneCamera;
using gfx::SceneLight;
using gfx::SceneMatrix;
using gfx::SceneMorph;
using gfx::MaterialProperty;
using gfx::MaterialTexture;
using gfx::ScenePointerTarget;

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
		   name == "KHR_lights_punctual" || name == "KHR_node_visibility" || name == "EXT_mesh_gpu_instancing" ||
		   name == "KHR_texture_basisu" || name == "EXT_texture_webp" || name == "KHR_draco_mesh_compression" ||
		   name == "EXT_meshopt_compression" || name == "KHR_meshopt_compression";
}

// a number or string value of a flat json object (an extension cgltf leaves as json), without its quotes
[[nodiscard]] std::optional<std::string> JsonValue(std::string_view json, std::string_view key)
{
	std::string compact(json);
	std::erase_if(compact, [](unsigned char c) { return std::isspace(c) != 0; });
	auto at = compact.find(std::format("\"{}\":", key));
	if (at == std::string::npos)
		return std::nullopt;
	auto begin = at + key.size() + 3;
	auto end = compact.find_first_of(",}", begin);
	auto value = compact.substr(begin, end == std::string::npos ? std::string::npos : end - begin);
	if (value.size() >= 2 && value.front() == '"' && value.back() == '"')
		value = value.substr(1, value.size() - 2);
	return value;
}

// buffer views compressed with meshopt (EXT_meshopt_compression, which cgltf parses, or KHR_meshopt_compression,
// which it leaves as json), decoded into memory that cgltf owns (cgltf_buffer_view::data, freed with the data). an
// error message if one can't be.
[[nodiscard]] std::optional<std::string> DecodeMeshopt(cgltf_data& data)
{
	for (cgltf_size viewIt = 0; viewIt < data.buffer_views_count; viewIt++)
	{
		auto& view = data.buffer_views[viewIt];
		cgltf_meshopt_compression compression{};
		std::string filter = "NONE";
		if (view.has_meshopt_compression)
		{
			compression = view.meshopt_compression;
		}
		else
		{
			const char* json = nullptr;
			for (cgltf_size extensionIt = 0; extensionIt < view.extensions_count; extensionIt++)
				if (view.extensions[extensionIt].name != nullptr &&
					std::string_view(view.extensions[extensionIt].name) == "KHR_meshopt_compression")
					json = view.extensions[extensionIt].data;
			if (json == nullptr)
				continue;
			auto number = [json](std::string_view key) -> cgltf_size
			{
				auto value = JsonValue(json, key);
				return value ? std::strtoull(value->c_str(), nullptr, 10) : 0;
			};
			auto buffer = number("buffer");
			if (!JsonValue(json, "buffer") || buffer >= data.buffers_count)
				return std::format("buffer view {}: KHR_meshopt_compression without a valid buffer", viewIt);
			compression.buffer = &data.buffers[buffer];
			compression.offset = number("byteOffset");
			compression.size = number("byteLength");
			compression.stride = number("byteStride");
			compression.count = number("count");
			auto mode = JsonValue(json, "mode").value_or("");
			compression.mode = mode == "ATTRIBUTES"  ? cgltf_meshopt_compression_mode_attributes
							 : mode == "TRIANGLES" ? cgltf_meshopt_compression_mode_triangles
							 : mode == "INDICES"   ? cgltf_meshopt_compression_mode_indices
												   : cgltf_meshopt_compression_mode_invalid;
			filter = JsonValue(json, "filter").value_or("NONE");
		}
		switch (compression.filter)
		{
		case cgltf_meshopt_compression_filter_octahedral: filter = "OCTAHEDRAL"; break;
		case cgltf_meshopt_compression_filter_quaternion: filter = "QUATERNION"; break;
		case cgltf_meshopt_compression_filter_exponential: filter = "EXPONENTIAL"; break;
		default: break;
		}

		if (compression.buffer == nullptr || compression.buffer->data == nullptr ||
			compression.offset + compression.size > compression.buffer->size)
			return std::format("buffer view {}: its meshopt compressed buffer isn't loaded", viewIt);

		const auto* source = static_cast<const unsigned char*>(compression.buffer->data) + compression.offset;
		auto size = compression.count * compression.stride;
		std::unique_ptr<void, void (*)(void*)> decoded(std::malloc(std::max<size_t>(size, 1)), std::free);
		int result = -1;
		switch (compression.mode)
		{
		case cgltf_meshopt_compression_mode_attributes:
			result = meshopt_decodeVertexBuffer(decoded.get(), compression.count, compression.stride, source, compression.size);
			break;
		case cgltf_meshopt_compression_mode_triangles:
			result = meshopt_decodeIndexBuffer(decoded.get(), compression.count, compression.stride, source, compression.size);
			break;
		case cgltf_meshopt_compression_mode_indices:
			result = meshopt_decodeIndexSequence(decoded.get(), compression.count, compression.stride, source, compression.size);
			break;
		default: break;
		}
		if (result != 0)
			return std::format("buffer view {}: its meshopt compressed data doesn't decode", viewIt);

		if (filter == "OCTAHEDRAL")
			meshopt_decodeFilterOct(decoded.get(), compression.count, compression.stride);
		else if (filter == "QUATERNION")
			meshopt_decodeFilterQuat(decoded.get(), compression.count, compression.stride);
		else if (filter == "EXPONENTIAL")
			meshopt_decodeFilterExp(decoded.get(), compression.count, compression.stride);
		else if (filter == "COLOR")
			meshopt_decodeFilterColor(decoded.get(), compression.count, compression.stride);
		else if (filter != "NONE")
			return std::format("buffer view {}: meshopt filter {} isn't supported", viewIt, filter);

		view.data = decoded.release();
	}
	return std::nullopt;
}

// a KHR_draco_mesh_compression primitive, decoded: its triangle list, and the values of its attributes, by the accessor
// they replace (as many floats per vertex as the accessor has components, normalized integers as floats in [0, 1])
struct DracoPrimitive
{
	std::vector<uint32_t> indices;
	core::UnorderedMap<const cgltf_accessor*, std::vector<float>> values;
};

[[nodiscard]] std::expected<DracoPrimitive, std::string> DecodeDraco(const cgltf_primitive& primitive, const cgltf_data& data)
{
	const auto& compression = primitive.draco_mesh_compression;
	const auto* bytes = reinterpret_cast<const char*>(cgltf_buffer_view_data(compression.buffer_view));
	if (bytes == nullptr)
		return std::unexpected("its draco compressed buffer isn't loaded");

	draco::DecoderBuffer buffer;
	buffer.Init(bytes, compression.buffer_view->size);
	draco::Decoder decoder;
	auto decoded = decoder.DecodeMeshFromBuffer(&buffer);
	if (!decoded.ok())
		return std::unexpected(std::format("its draco data doesn't decode: {}", decoded.status().error_msg_string()));
	auto mesh = std::move(decoded).value();

	DracoPrimitive result;
	result.indices.reserve(static_cast<size_t>(mesh->num_faces()) * 3);
	for (draco::FaceIndex faceIt(0); faceIt < mesh->num_faces(); ++faceIt)
		for (auto corner : mesh->face(faceIt))
			result.indices.push_back(corner.value());

	// the extension's attributes name the draco attribute ids (which cgltf turns into accessor pointers by index)
	for (cgltf_size attributeIt = 0; attributeIt < primitive.attributes_count; attributeIt++)
	{
		const auto& attribute = primitive.attributes[attributeIt];
		for (cgltf_size dracoIt = 0; dracoIt < compression.attributes_count; dracoIt++)
		{
			const auto& dracoAttribute = compression.attributes[dracoIt];
			if (dracoAttribute.name == nullptr || attribute.name == nullptr || std::string_view(dracoAttribute.name) != attribute.name)
				continue;
			const auto* source = mesh->GetAttributeByUniqueId(static_cast<uint32_t>(dracoAttribute.data - data.accessors));
			if (source == nullptr)
				return std::unexpected(std::format("its draco data has no attribute {}", attribute.name));
			auto components = cgltf_num_components(attribute.data->type);
			auto& values = result.values[attribute.data];
			values.resize(mesh->num_points() * components);
			for (draco::PointIndex pointIt(0); pointIt < mesh->num_points(); ++pointIt)
				source->ConvertValue<float>(source->mapped_index(pointIt), static_cast<int8_t>(components), &values[pointIt.value() * components]);
		}
	}
	return result;
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

[[nodiscard]] Matrix Multiply(const Matrix& a, const Matrix& b)
{
	Matrix product{};
	for (int col = 0; col < 4; col++)
		for (int row = 0; row < 4; row++)
			for (int k = 0; k < 4; k++)
				product[(col * 4) + row] += a[(k * 4) + row] * b[(col * 4) + k];
	return product;
}

// EXT_mesh_gpu_instancing: each instance's transform in its node's space, translation * rotation * scale (each
// attribute optional; normalized integer rotations are unpacked by cgltf). nullopt if an attribute can't be read.
[[nodiscard]] std::optional<std::vector<Matrix>> InstanceTransforms(const cgltf_mesh_gpu_instancing& instancing)
{
	if (instancing.attributes_count == 0)
		return std::nullopt;

	auto count = instancing.attributes[0].data->count;
	std::vector<float> translations(count * 3, 0.0F);
	std::vector<float> rotations(count * 4, 0.0F);
	std::vector<float> scales(count * 3, 1.0F);
	for (size_t i = 0; i < count; i++)
		rotations[(4 * i) + 3] = 1.0F;

	for (cgltf_size attributeIt = 0; attributeIt < instancing.attributes_count; attributeIt++)
	{
		const auto& attribute = instancing.attributes[attributeIt];
		std::string_view name = attribute.name != nullptr ? attribute.name : "";
		auto* values = name == "TRANSLATION" ? &translations : name == "ROTATION" ? &rotations : name == "SCALE" ? &scales : nullptr;
		if (values == nullptr)
			continue; // e.g. _FEATURE_ID_0 (EXT_instance_features)
		if (attribute.data->count != count ||
			cgltf_accessor_unpack_floats(attribute.data, values->data(), values->size()) != values->size())
			return std::nullopt;
	}

	std::vector<Matrix> transforms(count);
	for (size_t i = 0; i < count; i++)
	{
		const auto* t = &translations[3 * i];
		const auto* q = &rotations[4 * i];
		const auto* scale = &scales[3 * i];
		auto length = std::sqrt((q[0] * q[0]) + (q[1] * q[1]) + (q[2] * q[2]) + (q[3] * q[3]));
		auto inverse = length > 0.0F ? 1.0F / length : 0.0F;
		auto x = q[0] * inverse;
		auto y = q[1] * inverse;
		auto z = q[2] * inverse;
		auto w = length > 0.0F ? q[3] * inverse : 1.0F;

		transforms[i] = {
			(1.0F - (2.0F * ((y * y) + (z * z)))) * scale[0], 2.0F * ((x * y) + (z * w)) * scale[0], 2.0F * ((x * z) - (y * w)) * scale[0], 0.0F,
			2.0F * ((x * y) - (z * w)) * scale[1], (1.0F - (2.0F * ((x * x) + (z * z)))) * scale[1], 2.0F * ((y * z) + (x * w)) * scale[1], 0.0F,
			2.0F * ((x * z) + (y * w)) * scale[2], 2.0F * ((y * z) - (x * w)) * scale[2], (1.0F - (2.0F * ((x * x) + (y * y)))) * scale[2], 0.0F,
			t[0], t[1], t[2], 1.0F};
	}
	return transforms;
}

// a camera node's camera, in world space: it looks along its node's -z, with +y up
[[nodiscard]] SceneCamera SceneCameraOf(const cgltf_node& node, const cgltf_data& data)
{
	const auto& camera = *node.camera;

	Matrix world{};
	cgltf_node_transform_world(&node, world.data());
	auto unit = [](Vec3 v) -> std::array<float, 3>
	{
		auto length = Length(v);
		if (!(length > 0.0) || !IsFinite(v))
			return {0.0F, 0.0F, 0.0F};
		return {static_cast<float>(v[0] / length), static_cast<float>(v[1] / length), static_cast<float>(v[2] / length)};
	};

	SceneCamera result{
		.name = camera.name != nullptr ? camera.name
				: node.name != nullptr ? node.name
									   : std::format("camera {}", &camera - data.cameras),
		.position = {world[12], world[13], world[14]},
		.forward = unit({-world[8], -world[9], -world[10]}),
		.up = unit({world[4], world[5], world[6]}),
		.node = static_cast<int32_t>(&node - data.nodes),
		.source = static_cast<uint32_t>(&camera - data.cameras)};
	if (camera.type == cgltf_camera_type_orthographic)
	{
		result.orthographic = true;
		result.ymag = camera.data.orthographic.ymag;
		if (camera.data.orthographic.xmag > 0.0F && camera.data.orthographic.ymag > 0.0F)
			result.aspectRatio = camera.data.orthographic.xmag / camera.data.orthographic.ymag;
		result.znear = camera.data.orthographic.znear;
		result.zfar = camera.data.orthographic.zfar;
	}
	else
	{
		result.yfov = camera.data.perspective.yfov;
		if (camera.data.perspective.has_aspect_ratio != 0 && camera.data.perspective.aspect_ratio > 0.0F)
			result.aspectRatio = camera.data.perspective.aspect_ratio;
		result.znear = camera.data.perspective.znear;
		result.zfar = camera.data.perspective.has_zfar != 0 ? camera.data.perspective.zfar : 0.0F;
	}
	return result;
}

// a light node's light (KHR_lights_punctual), in world space: it shines along its node's -z
[[nodiscard]] SceneLight SceneLightOf(const cgltf_node& node, const cgltf_data& data)
{
	const auto& light = *node.light;

	Matrix world{};
	cgltf_node_transform_world(&node, world.data());
	Vec3 direction{-world[8], -world[9], -world[10]};
	auto length = Length(direction);

	SceneLight result{
		.name = light.name != nullptr ? light.name : node.name != nullptr ? node.name : std::format("light {}", &light - data.lights),
		.type = light.type == cgltf_light_type_point  ? SceneLight::Type::kPoint
				: light.type == cgltf_light_type_spot ? SceneLight::Type::kSpot
													  : SceneLight::Type::kDirectional,
		.position = {world[12], world[13], world[14]},
		.color = {light.color[0], light.color[1], light.color[2]},
		.intensity = light.intensity,
		.range = light.range,
		.innerConeAngle = light.spot_inner_cone_angle,
		.outerConeAngle = light.spot_outer_cone_angle,
		.source = static_cast<uint32_t>(&light - data.lights)};
	if (length > 0.0 && IsFinite(direction))
		for (size_t axis = 0; axis < 3; axis++)
			result.direction[axis] = static_cast<float>(direction[axis] / length);
	return result;
}

// by the upper 3x3 only, for directions such as tangents
[[nodiscard]] Vec3 TransformDirection(const Matrix& m, const Vec3& v)
{
	return {
		(m[0] * v[0]) + (m[4] * v[1]) + (m[8] * v[2]),
		(m[1] * v[0]) + (m[5] * v[1]) + (m[9] * v[2]),
		(m[2] * v[0]) + (m[6] * v[1]) + (m[10] * v[2])};
}

[[nodiscard]] Vec3 TransformNormal(const std::array<double, 9>& n, const Vec3& v)
{
	// rows of the cofactor matrix
	return {
		(n[0] * v[0]) + (n[1] * v[1]) + (n[2] * v[2]),
		(n[3] * v[0]) + (n[4] * v[1]) + (n[5] * v[2]),
		(n[6] * v[0]) + (n[7] * v[1]) + (n[8] * v[2])};
}

// KHR_texture_transform as a 2x3 matrix (see TextureRef::transform): offset * rotation * scale, with the rotation as
// the extension defines it (counter-clockwise in uv space, whose v points down)
[[nodiscard]] std::array<float, 6> TransformMatrix(const cgltf_texture_transform& t)
{
	return TextureTransform({t.offset[0], t.offset[1]}, t.rotation, {t.scale[0], t.scale[1]});
}

// a gltf sampler as rhi's (default: repeat, linear with mipmaps, anisotropic, as the spec suggests for no sampler)
[[nodiscard]] rhi::SamplerDesc SamplerOf(const cgltf_sampler* sampler)
{
	rhi::SamplerDesc desc{.maxAnisotropy = TextureRef::kDefaultMaxAnisotropy};
	if (sampler == nullptr)
		return desc;

	auto wrap = [](cgltf_int mode)
	{
		switch (mode)
		{
		case 33071: return rhi::AddressMode::kClampToEdge; // CLAMP_TO_EDGE
		case 33648: return rhi::AddressMode::kMirroredRepeat; // MIRRORED_REPEAT
		default: return rhi::AddressMode::kRepeat; // REPEAT
		}
	};
	desc.addressModeU = wrap(sampler->wrap_s);
	desc.addressModeV = wrap(sampler->wrap_t);

	// filters: 9728 NEAREST, 9729 LINEAR, and for min also 9984 NEAREST_MIPMAP_NEAREST, 9985 LINEAR_MIPMAP_NEAREST,
	// 9986 NEAREST_MIPMAP_LINEAR, 9987 LINEAR_MIPMAP_LINEAR. 0 is undefined: linear (with linear mipmaps)
	if (sampler->mag_filter == 9728)
		desc.magFilter = rhi::Filter::kNearest;
	switch (sampler->min_filter)
	{
	case 9728: desc.minFilter = rhi::Filter::kNearest; desc.maxLod = 0.0F; break; // no mipmaps
	case 9729: desc.maxLod = 0.0F; break;
	case 9984: desc.minFilter = rhi::Filter::kNearest; desc.mipmapFilter = rhi::Filter::kNearest; break;
	case 9985: desc.mipmapFilter = rhi::Filter::kNearest; break;
	case 9986: desc.minFilter = rhi::Filter::kNearest; break;
	default: break;
	}
	// anisotropic filtering only where it filters linearly
	if (desc.magFilter != rhi::Filter::kLinear || desc.minFilter != rhi::Filter::kLinear)
		desc.maxAnisotropy = 1.0F;
	return desc;
}

// the image files of a file's textures: external ones are resolved next to the file, embedded ones (in a buffer view,
// or a data uri) are written to the embedded image directory, once per image
// an embedded image's bytes (in a buffer view, or a base64 data uri) and its mime type, or why they can't be read. the
// buffers must be loaded.
struct EmbeddedBytes
{
	std::vector<std::byte> bytes;
	std::string mimeType;
};

[[nodiscard]] std::expected<EmbeddedBytes, std::string> ReadEmbedded(const cgltf_image& image)
{
	EmbeddedBytes result{.mimeType = image.mime_type != nullptr ? image.mime_type : ""};
	if (image.buffer_view != nullptr)
	{
		const auto* data = reinterpret_cast<const std::byte*>(cgltf_buffer_view_data(image.buffer_view));
		if (data == nullptr)
			return std::unexpected("its buffer isn't loaded");
		result.bytes.assign(data, data + image.buffer_view->size);
		return result;
	}
	if (image.uri != nullptr && IsDataUri(image.uri))
	{
		std::string_view uri(image.uri);
		auto comma = uri.find(',');
		auto header = uri.substr(0, comma);
		if (comma == std::string_view::npos || !header.ends_with(";base64"))
			return std::unexpected("a data uri that isn't base64");
		result.mimeType = header.substr(5, header.size() - 5 - 7); // between "data:" and ";base64"
		auto base64 = uri.substr(comma + 1);
		auto padding = std::ranges::count(base64.substr(base64.size() >= 2 ? base64.size() - 2 : 0), '=');
		auto size = ((base64.size() / 4) * 3) - static_cast<size_t>(padding);
		cgltf_options options{};
		void* out = nullptr;
		if (cgltf_load_buffer_base64(&options, size, base64.data(), &out) != cgltf_result_success)
			return std::unexpected("its data uri doesn't decode");
		std::unique_ptr<void, void (*)(void*)> decoded(out, std::free);
		result.bytes.assign(static_cast<const std::byte*>(out), static_cast<const std::byte*>(out) + size);
		return result;
	}
	return std::unexpected("it is neither in a buffer view nor a data uri");
}

// the image file of each texture, or for an image the gltf file embeds, the gltf file and the image's index (see
// TextureRef::embeddedImage), which the texture loader reads it from (see EmbeddedImage)
class Images
{
public:
	struct Resolved
	{
		std::filesystem::path path; // empty if it can't be loaded
		std::optional<uint32_t> embeddedImage;
	};

	Images(const cgltf_data& data, const std::filesystem::path& path, mesh::Stats& stats)
		: myData(data)
		, myPath(path)
		, myBaseDir(path.parent_path())
		, myStats(stats)
		, myResolved(data.images_count)
		, myErrors(data.images_count)
	{}

	// a texture's image, or an empty path (counted as missing) if it has none that can be loaded
	[[nodiscard]] Resolved Resolve(const cgltf_texture_view& view, std::string_view material)
	{
		const auto* texture = view.texture;
		if (texture == nullptr)
			return {};

		// its plain image if it has one (a fallback for the extensions'), else its KTX2 (KHR_texture_basisu) or WebP
		// (EXT_texture_webp) image, which image::Import decodes too
		const auto* image = texture->image != nullptr ? texture->image : texture->basisu_image != nullptr ? texture->basisu_image : texture->webp_image;
		if (image == nullptr)
		{
			Missing(material, "the texture has no image");
			return {};
		}

		auto index = static_cast<size_t>(image - myData.images);
		if (!myResolved[index])
			myResolved[index] = InternalResolve(*image, index);
		if (myResolved[index]->path.empty())
			Missing(material, myErrors[index]);
		return *myResolved[index];
	}

private:
	void Missing(std::string_view material, std::string_view why)
	{
		myStats.missingTextures++;
		myStats.warnings.emplace_back(std::format("material {}: texture not loaded: {}", material, why));
	}

	[[nodiscard]] Resolved InternalResolve(const cgltf_image& image, size_t index)
	{
		auto& error = myErrors[index];

		if (image.uri != nullptr && !IsDataUri(image.uri))
		{
			auto path = myBaseDir / UriPath(image.uri);
			if (std::error_code ec; !std::filesystem::is_regular_file(path, ec))
			{
				error = std::format("{} not found", path.string());
				return {};
			}
			return {.path = path};
		}

		// embedded: readable, and of a type image::Import decodes
		auto embedded = ReadEmbedded(image);
		if (!embedded)
		{
			error = embedded.error();
			return {};
		}
		static constexpr std::array<std::string_view, 4> kMimeTypes{"image/png", "image/jpeg", "image/webp", "image/ktx2"};
		if (!std::ranges::contains(kMimeTypes, embedded->mimeType))
		{
			error = std::format("embedded {} images aren't supported", embedded->mimeType.empty() ? "untyped" : embedded->mimeType);
			return {};
		}
		return {.path = myPath, .embeddedImage = static_cast<uint32_t>(index)};
	}

	const cgltf_data& myData;
	std::filesystem::path myPath;
	std::filesystem::path myBaseDir;
	mesh::Stats& myStats;
	std::vector<std::optional<Resolved>> myResolved;
	std::vector<std::string> myErrors;
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
	rhi::PrimitiveTopology topology = rhi::PrimitiveTopology::kTriangleList;
	// its instances (see mesh::Submesh::firstInstance): 0, the identity, for parts whose vertices are in world space
	uint32_t firstInstance = 0;
	uint32_t instanceCount = 1;
	uint32_t mirroredInstanceCount = 0;
	int32_t skin = -1; // see mesh::Submesh::skin
	// animated morph targets (see mesh::Submesh::morphTargetCount): a row of morphTargets deltas per vertex
	uint32_t morphTargets = 0;
	uint32_t morphWeightBase = 0;
	std::vector<VertexP3fN3fTa4fT014fC4f> vertices;
	std::vector<SkinVertex> skinVertices; // parallel to vertices if skinned, else empty
	std::vector<MorphDelta> morphDeltas; // morphTargets per vertex, if any
	std::vector<uint32_t> indices;
};

// MikkTSpace tangents for a triangle part (see mesh::GenerateTangents), keeping its skin vertices and morph deltas
// with their vertices. returns how many vertices it has after, or 0 if MikkTSpace failed.
size_t GenerateTangents(Part& part, uint32_t texCoordSet)
{
	auto sources = mesh::GenerateTangents(part.vertices, part.indices, texCoordSet);
	if (sources.empty())
		return 0;

	if (!part.skinVertices.empty())
	{
		std::vector<SkinVertex> skinVertices;
		skinVertices.reserve(sources.size());
		for (auto source : sources)
			skinVertices.push_back(part.skinVertices[source]);
		part.skinVertices = std::move(skinVertices);
	}
	if (part.morphTargets > 0)
	{
		std::vector<MorphDelta> morphDeltas;
		morphDeltas.reserve(sources.size() * part.morphTargets);
		for (auto source : sources)
			std::ranges::copy(
				std::span(part.morphDeltas).subspan(static_cast<size_t>(source) * part.morphTargets, part.morphTargets),
				std::back_inserter(morphDeltas));
		part.morphDeltas = std::move(morphDeltas);
	}
	return part.vertices.size();
}

// KHR_animation_pointer: each animation's channels' pointers (empty where a channel has none), read from the json with
// cgltf's jsmn, since cgltf skips a channel target's extensions. JSON pointer escapes (~1, ~0) are undone per segment
// by ParsePointer.
[[nodiscard]] std::vector<std::vector<std::string>> AnimationPointers(const cgltf_data& data)
{
	std::vector<std::vector<std::string>> result(data.animations_count);
	for (cgltf_size animationIt = 0; animationIt < data.animations_count; animationIt++)
		result[animationIt].resize(data.animations[animationIt].channels_count);
	if (data.json == nullptr || data.json_size == 0)
		return result;

	jsmn_parser parser;
	jsmn_init(&parser);
	auto count = jsmn_parse(&parser, data.json, data.json_size, nullptr, 0);
	if (count <= 0)
		return result;
	std::vector<jsmntok_t> tokens(static_cast<size_t>(count));
	jsmn_init(&parser);
	if (jsmn_parse(&parser, data.json, data.json_size, tokens.data(), tokens.size()) < 0)
		return result;

	std::string_view json(data.json, data.json_size);
	auto text = [&](int token) { return json.substr(tokens[token].start, tokens[token].end - tokens[token].start); };
	// the token after token's subtree (an object's keys have their value as their one child)
	auto skip = [&](int token)
	{
		for (int pending = 1; pending > 0 && std::cmp_less(token, tokens.size()); token++)
			pending += tokens[token].size - 1;
		return token;
	};
	auto member = [&](int object, std::string_view key) -> int
	{
		if (object < 0 || tokens[object].type != JSMN_OBJECT)
			return -1;
		auto token = object + 1;
		for (int keyIt = 0; keyIt < tokens[object].size && std::cmp_less(token + 1, tokens.size()); keyIt++)
		{
			if (tokens[token].type == JSMN_STRING && text(token) == key)
				return token + 1;
			token = skip(token);
		}
		return -1;
	};
	auto elements = [&](int array)
	{
		std::vector<int> result;
		if (array < 0 || tokens[array].type != JSMN_ARRAY)
			return result;
		for (int elementIt = 0, token = array + 1; elementIt < tokens[array].size; elementIt++, token = skip(token))
			result.push_back(token);
		return result;
	};

	auto animations = elements(member(0, "animations"));
	for (size_t animationIt = 0; animationIt < std::min(animations.size(), result.size()); animationIt++)
	{
		auto channels = elements(member(animations[animationIt], "channels"));
		for (size_t channelIt = 0; channelIt < std::min(channels.size(), result[animationIt].size()); channelIt++)
		{
			auto pointer = member(member(member(member(channels[channelIt], "target"), "extensions"), "KHR_animation_pointer"), "pointer");
			if (pointer >= 0 && tokens[pointer].type == JSMN_STRING)
				result[animationIt][channelIt] = std::string(text(pointer));
		}
	}
	return result;
}

// what a KHR_animation_pointer pointer targets, of what the renderer can apply
struct ParsedPointer
{
	enum class Kind : uint8_t
	{
		kNone, // not one of those
		kNodeTranslation,
		kNodeRotation,
		kNodeScale,
		kNodeWeights,
		kNodeVisibility,
		kMaterial, // property: a MaterialProperty
		kTextureOffset, // property: a MaterialTexture
		kTextureRotation,
		kTextureScale,
		kLight, // property: a LightProperty
		kCamera, // property: a CameraProperty
	};

	Kind kind = Kind::kNone;
	uint32_t index = 0; // the node, material, light or camera
	uint16_t property = 0;
	int32_t target = -1; // its ScenePointerTarget, for the material and texture ones
};

// the material properties' pointers below /materials/<index>/
constexpr std::array<std::pair<std::string_view, MaterialProperty>, 29> kMaterialPointers{{
	{"extensions/KHR_materials_emissive_strength/emissiveStrength", MaterialProperty::kEmissiveStrength},
	{"extensions/KHR_materials_anisotropy/anisotropyRotation", MaterialProperty::kAnisotropyRotation},
	{"pbrMetallicRoughness/baseColorFactor", MaterialProperty::kBaseColor},
	{"pbrMetallicRoughness/metallicFactor", MaterialProperty::kMetallic},
	{"pbrMetallicRoughness/roughnessFactor", MaterialProperty::kRoughness},
	{"emissiveFactor", MaterialProperty::kEmissive},
	{"alphaCutoff", MaterialProperty::kAlphaCutoff},
	{"normalTexture/scale", MaterialProperty::kNormalScale},
	{"occlusionTexture/strength", MaterialProperty::kOcclusionStrength},
	{"extensions/KHR_materials_specular/specularFactor", MaterialProperty::kSpecular},
	{"extensions/KHR_materials_specular/specularColorFactor", MaterialProperty::kSpecularColor},
	{"extensions/KHR_materials_ior/ior", MaterialProperty::kIor},
	{"extensions/KHR_materials_clearcoat/clearcoatFactor", MaterialProperty::kClearcoat},
	{"extensions/KHR_materials_clearcoat/clearcoatRoughnessFactor", MaterialProperty::kClearcoatRoughness},
	{"extensions/KHR_materials_clearcoat/clearcoatNormalTexture/scale", MaterialProperty::kClearcoatNormalScale},
	{"extensions/KHR_materials_sheen/sheenColorFactor", MaterialProperty::kSheenColor},
	{"extensions/KHR_materials_sheen/sheenRoughnessFactor", MaterialProperty::kSheenRoughness},
	{"extensions/KHR_materials_transmission/transmissionFactor", MaterialProperty::kTransmission},
	{"extensions/KHR_materials_volume/thicknessFactor", MaterialProperty::kThickness},
	{"extensions/KHR_materials_volume/attenuationColor", MaterialProperty::kAttenuationColor},
	{"extensions/KHR_materials_volume/attenuationDistance", MaterialProperty::kAttenuationDistance},
	{"extensions/KHR_materials_dispersion/dispersion", MaterialProperty::kDispersion},
	{"extensions/KHR_materials_anisotropy/anisotropyStrength", MaterialProperty::kAnisotropy},
	{"extensions/KHR_materials_iridescence/iridescenceFactor", MaterialProperty::kIridescence},
	{"extensions/KHR_materials_iridescence/iridescenceIor", MaterialProperty::kIridescenceIor},
	{"extensions/KHR_materials_iridescence/iridescenceThicknessMinimum", MaterialProperty::kIridescenceThicknessMin},
	{"extensions/KHR_materials_iridescence/iridescenceThicknessMaximum", MaterialProperty::kIridescenceThicknessMax},
	{"extensions/KHR_materials_diffuse_transmission/diffuseTransmissionFactor", MaterialProperty::kDiffuseTransmission},
	{"extensions/KHR_materials_diffuse_transmission/diffuseTransmissionColorFactor", MaterialProperty::kDiffuseTransmissionColor},
}};

// a light's or camera's value as the file has it
[[nodiscard]] std::vector<float> LightPropertyRest(const cgltf_light& light, LightProperty property)
{
	switch (property)
	{
	case LightProperty::kColor: return {light.color[0], light.color[1], light.color[2]};
	case LightProperty::kIntensity: return {light.intensity};
	case LightProperty::kRange: return {light.range};
	case LightProperty::kInnerConeAngle: return {light.spot_inner_cone_angle};
	case LightProperty::kOuterConeAngle: return {light.spot_outer_cone_angle};
	}
	return {};
}

[[nodiscard]] std::vector<float> CameraPropertyRest(const cgltf_camera& camera, CameraProperty property)
{
	bool orthographic = camera.type == cgltf_camera_type_orthographic;
	const auto& perspective = camera.data.perspective;
	const auto& ortho = camera.data.orthographic;
	switch (property)
	{
	case CameraProperty::kYfov: return {orthographic ? 0.0F : perspective.yfov};
	case CameraProperty::kAspectRatio: return {!orthographic && perspective.has_aspect_ratio ? perspective.aspect_ratio : 0.0F};
	case CameraProperty::kZnear: return {orthographic ? ortho.znear : perspective.znear};
	case CameraProperty::kZfar: return {orthographic ? ortho.zfar : (perspective.has_zfar ? perspective.zfar : 0.0F)};
	case CameraProperty::kXmag: return {orthographic ? ortho.xmag : 0.0F};
	case CameraProperty::kYmag: return {orthographic ? ortho.ymag : 0.0F};
	}
	return {};
}

// the material textures' pointers below /materials/<index>/, before /extensions/KHR_texture_transform/...
constexpr std::array<std::pair<std::string_view, MaterialTexture>, 19> kTexturePointers{{
	{"pbrMetallicRoughness/baseColorTexture", MaterialTexture::kBaseColor},
	{"pbrMetallicRoughness/metallicRoughnessTexture", MaterialTexture::kMetallicRoughness},
	{"normalTexture", MaterialTexture::kNormal},
	{"occlusionTexture", MaterialTexture::kOcclusion},
	{"emissiveTexture", MaterialTexture::kEmissive},
	{"extensions/KHR_materials_specular/specularTexture", MaterialTexture::kSpecular},
	{"extensions/KHR_materials_specular/specularColorTexture", MaterialTexture::kSpecularColor},
	{"extensions/KHR_materials_clearcoat/clearcoatTexture", MaterialTexture::kClearcoat},
	{"extensions/KHR_materials_clearcoat/clearcoatRoughnessTexture", MaterialTexture::kClearcoatRoughness},
	{"extensions/KHR_materials_clearcoat/clearcoatNormalTexture", MaterialTexture::kClearcoatNormal},
	{"extensions/KHR_materials_sheen/sheenColorTexture", MaterialTexture::kSheenColor},
	{"extensions/KHR_materials_sheen/sheenRoughnessTexture", MaterialTexture::kSheenRoughness},
	{"extensions/KHR_materials_transmission/transmissionTexture", MaterialTexture::kTransmission},
	{"extensions/KHR_materials_volume/thicknessTexture", MaterialTexture::kThickness},
	{"extensions/KHR_materials_anisotropy/anisotropyTexture", MaterialTexture::kAnisotropy},
	{"extensions/KHR_materials_iridescence/iridescenceTexture", MaterialTexture::kIridescence},
	{"extensions/KHR_materials_iridescence/iridescenceThicknessTexture", MaterialTexture::kIridescenceThickness},
	{"extensions/KHR_materials_diffuse_transmission/diffuseTransmissionTexture", MaterialTexture::kDiffuseTransmission},
	{"extensions/KHR_materials_diffuse_transmission/diffuseTransmissionColorTexture", MaterialTexture::kDiffuseTransmissionColor},
}};

[[nodiscard]] ParsedPointer ParsePointer(std::string_view pointer, const cgltf_data& data)
{
	// its segments, unescaped
	std::vector<std::string> segments;
	for (size_t begin = 0; begin <= pointer.size();)
	{
		auto end = std::min(pointer.find('/', begin), pointer.size());
		std::string segment(pointer.substr(begin, end - begin));
		for (size_t at = 0; (at = segment.find('~', at)) != std::string::npos && at + 1 < segment.size(); at++)
			segment.replace(at, 2, segment[at + 1] == '1' ? "/" : "~");
		segments.push_back(std::move(segment));
		begin = end + 1;
	}
	if (segments.size() < 4 || !segments[0].empty())
		return {};
	using Kind = ParsedPointer::Kind;
	// KHR_lights_punctual's lights: /extensions/KHR_lights_punctual/lights/<index>/...
	if (segments.size() >= 6 && segments[1] == "extensions" && segments[2] == "KHR_lights_punctual" && segments[3] == "lights")
	{
		uint32_t light = 0;
		if (auto [end, error] = std::from_chars(segments[4].data(), segments[4].data() + segments[4].size(), light);
			error != std::errc{} || end != segments[4].data() + segments[4].size() || light >= data.lights_count)
			return {};
		std::string property;
		for (size_t segmentIt = 5; segmentIt < segments.size(); segmentIt++)
			property += (segmentIt > 5 ? "/" : "") + segments[segmentIt];
		for (auto [path, value] : std::array{
				 std::pair{std::string_view("color"), LightProperty::kColor},
				 std::pair{std::string_view("intensity"), LightProperty::kIntensity},
				 std::pair{std::string_view("range"), LightProperty::kRange},
				 std::pair{std::string_view("spot/innerConeAngle"), LightProperty::kInnerConeAngle},
				 std::pair{std::string_view("spot/outerConeAngle"), LightProperty::kOuterConeAngle}})
			if (property == path)
				return {.kind = Kind::kLight, .index = light, .property = std::to_underlying(value)};
		return {};
	}
	uint32_t index = 0;
	if (auto [end, error] = std::from_chars(segments[2].data(), segments[2].data() + segments[2].size(), index);
		error != std::errc{} || end != segments[2].data() + segments[2].size())
		return {};
	std::string rest;
	for (size_t segmentIt = 3; segmentIt < segments.size(); segmentIt++)
		rest += (segmentIt > 3 ? "/" : "") + segments[segmentIt];

	if (segments[1] == "nodes" && index < data.nodes_count)
	{
		auto kind = rest == "translation" ? Kind::kNodeTranslation
				  : rest == "rotation"    ? Kind::kNodeRotation
				  : rest == "scale"       ? Kind::kNodeScale
				  : rest == "weights"     ? Kind::kNodeWeights
				  : rest == "extensions/KHR_node_visibility/visible" ? Kind::kNodeVisibility
																	   : Kind::kNone;
		return {.kind = kind, .index = index};
	}
	if (segments[1] == "cameras" && index < data.cameras_count)
	{
		for (auto [path, property] : std::array{
				 std::pair{std::string_view("perspective/yfov"), CameraProperty::kYfov},
				 std::pair{std::string_view("perspective/aspectRatio"), CameraProperty::kAspectRatio},
				 std::pair{std::string_view("perspective/znear"), CameraProperty::kZnear},
				 std::pair{std::string_view("perspective/zfar"), CameraProperty::kZfar},
				 std::pair{std::string_view("orthographic/xmag"), CameraProperty::kXmag},
				 std::pair{std::string_view("orthographic/ymag"), CameraProperty::kYmag},
				 std::pair{std::string_view("orthographic/znear"), CameraProperty::kZnear},
				 std::pair{std::string_view("orthographic/zfar"), CameraProperty::kZfar}})
			if (rest == path)
				return {.kind = Kind::kCamera, .index = index, .property = std::to_underlying(property)};
		return {};
	}
	if (segments[1] == "materials" && index < data.materials_count)
	{
		for (auto [path, property] : kMaterialPointers)
			if (rest == path)
				return {.kind = Kind::kMaterial, .index = index, .property = std::to_underlying(property)};
		for (auto [suffix, kind] : std::array{
				 std::pair{std::string_view("/extensions/KHR_texture_transform/offset"), Kind::kTextureOffset},
				 std::pair{std::string_view("/extensions/KHR_texture_transform/rotation"), Kind::kTextureRotation},
				 std::pair{std::string_view("/extensions/KHR_texture_transform/scale"), Kind::kTextureScale}})
			if (rest.ends_with(suffix))
				for (auto [path, texture] : kTexturePointers)
					if (std::string_view(rest).substr(0, rest.size() - suffix.size()) == path)
						return {.kind = kind, .index = index, .property = std::to_underlying(texture)};
	}
	return {};
}

// a material property's value as the file has it (the spec's default without its extension), and what it is scaled by
[[nodiscard]] std::vector<float> MaterialPropertyRest(const cgltf_material& m, MaterialProperty property)
{
	const auto& pbr = m.pbr_metallic_roughness;
	bool metallicRoughness = m.has_pbr_metallic_roughness != 0;
	switch (property)
	{
	case MaterialProperty::kBaseColor:
		return metallicRoughness ? std::vector<float>(pbr.base_color_factor, pbr.base_color_factor + 4) : std::vector<float>{1, 1, 1, 1};
	case MaterialProperty::kEmissive: return {m.emissive_factor[0], m.emissive_factor[1], m.emissive_factor[2]};
	case MaterialProperty::kEmissiveStrength: return {m.has_emissive_strength ? m.emissive_strength.emissive_strength : 1.0F};
	case MaterialProperty::kAnisotropyRotation: return {m.has_anisotropy ? m.anisotropy.anisotropy_rotation : 0.0F};
	case MaterialProperty::kMetallic: return {metallicRoughness ? pbr.metallic_factor : 1.0F};
	case MaterialProperty::kRoughness: return {metallicRoughness ? pbr.roughness_factor : 1.0F};
	case MaterialProperty::kAlphaCutoff: return {m.alpha_cutoff};
	case MaterialProperty::kNormalScale: return {m.normal_texture.scale};
	case MaterialProperty::kOcclusionStrength: return {m.occlusion_texture.scale};
	case MaterialProperty::kSpecular: return {m.has_specular ? m.specular.specular_factor : 1.0F};
	case MaterialProperty::kSpecularColor:
		return m.has_specular ? std::vector<float>(m.specular.specular_color_factor, m.specular.specular_color_factor + 3) : std::vector<float>{1, 1, 1};
	case MaterialProperty::kIor: return {m.has_ior ? m.ior.ior : 1.5F};
	case MaterialProperty::kClearcoat: return {m.has_clearcoat ? m.clearcoat.clearcoat_factor : 0.0F};
	case MaterialProperty::kClearcoatRoughness: return {m.has_clearcoat ? m.clearcoat.clearcoat_roughness_factor : 0.0F};
	case MaterialProperty::kClearcoatNormalScale: return {m.has_clearcoat ? m.clearcoat.clearcoat_normal_texture.scale : 1.0F};
	case MaterialProperty::kSheenColor:
		return m.has_sheen ? std::vector<float>(m.sheen.sheen_color_factor, m.sheen.sheen_color_factor + 3) : std::vector<float>{0, 0, 0};
	case MaterialProperty::kSheenRoughness: return {m.has_sheen ? m.sheen.sheen_roughness_factor : 0.0F};
	case MaterialProperty::kTransmission: return {m.has_transmission ? m.transmission.transmission_factor : 0.0F};
	case MaterialProperty::kThickness: return {m.has_volume ? m.volume.thickness_factor : 0.0F};
	case MaterialProperty::kAttenuationColor:
		return m.has_volume ? std::vector<float>(m.volume.attenuation_color, m.volume.attenuation_color + 3) : std::vector<float>{1, 1, 1};
	case MaterialProperty::kAttenuationDistance:
		return {m.has_volume && std::isfinite(m.volume.attenuation_distance) && m.volume.attenuation_distance < 1e30F ? m.volume.attenuation_distance : 0.0F};
	case MaterialProperty::kDispersion: return {m.has_dispersion ? m.dispersion.dispersion : 0.0F};
	case MaterialProperty::kAnisotropy: return {m.has_anisotropy ? m.anisotropy.anisotropy_strength : 0.0F};
	case MaterialProperty::kIridescence: return {m.has_iridescence ? m.iridescence.iridescence_factor : 0.0F};
	case MaterialProperty::kIridescenceIor: return {m.has_iridescence ? m.iridescence.iridescence_ior : 1.3F};
	case MaterialProperty::kIridescenceThicknessMin: return {m.has_iridescence ? m.iridescence.iridescence_thickness_min : 100.0F};
	case MaterialProperty::kIridescenceThicknessMax: return {m.has_iridescence ? m.iridescence.iridescence_thickness_max : 400.0F};
	case MaterialProperty::kDiffuseTransmission:
		return {m.has_diffuse_transmission ? m.diffuse_transmission.diffuse_transmission_factor : 0.0F};
	case MaterialProperty::kDiffuseTransmissionColor:
		return m.has_diffuse_transmission ? std::vector<float>(m.diffuse_transmission.diffuse_transmission_color_factor, m.diffuse_transmission.diffuse_transmission_color_factor + 3)
										  : std::vector<float>{1, 1, 1};
	}
	return {};
}

// a material's texture view of a kind, as the file has it
[[nodiscard]] const cgltf_texture_view& MaterialTextureView(const cgltf_material& m, MaterialTexture texture)
{
	switch (texture)
	{
	case MaterialTexture::kBaseColor: return m.pbr_metallic_roughness.base_color_texture;
	case MaterialTexture::kMetallicRoughness: return m.pbr_metallic_roughness.metallic_roughness_texture;
	case MaterialTexture::kNormal: return m.normal_texture;
	case MaterialTexture::kOcclusion: return m.occlusion_texture;
	case MaterialTexture::kEmissive: return m.emissive_texture;
	case MaterialTexture::kSpecular: return m.specular.specular_texture;
	case MaterialTexture::kSpecularColor: return m.specular.specular_color_texture;
	case MaterialTexture::kClearcoat: return m.clearcoat.clearcoat_texture;
	case MaterialTexture::kClearcoatRoughness: return m.clearcoat.clearcoat_roughness_texture;
	case MaterialTexture::kClearcoatNormal: return m.clearcoat.clearcoat_normal_texture;
	case MaterialTexture::kSheenColor: return m.sheen.sheen_color_texture;
	case MaterialTexture::kSheenRoughness: return m.sheen.sheen_roughness_texture;
	case MaterialTexture::kTransmission: return m.transmission.transmission_texture;
	case MaterialTexture::kThickness: return m.volume.thickness_texture;
	case MaterialTexture::kAnisotropy: return m.anisotropy.anisotropy_texture;
	case MaterialTexture::kIridescence: return m.iridescence.iridescence_texture;
	case MaterialTexture::kIridescenceThickness: return m.iridescence.iridescence_thickness_texture;
	case MaterialTexture::kDiffuseTransmission: return m.diffuse_transmission.diffuse_transmission_texture;
	case MaterialTexture::kDiffuseTransmissionColor: return m.diffuse_transmission.diffuse_transmission_color_texture;
	}
	return m.normal_texture;
}

// the imported material's reference to that texture
[[nodiscard]] TextureRef& MaterialTextureRef(mesh::Material& m, MaterialTexture texture)
{
	switch (texture)
	{
	case MaterialTexture::kBaseColor: return m.diffuseTexture;
	case MaterialTexture::kMetallicRoughness: return m.metallicRoughnessTexture;
	case MaterialTexture::kNormal: return m.normalTexture;
	case MaterialTexture::kOcclusion: return m.occlusionTexture;
	case MaterialTexture::kEmissive: return m.emissiveTexture;
	case MaterialTexture::kSpecular: return m.specularTexture;
	case MaterialTexture::kSpecularColor: return m.specularColorTexture;
	case MaterialTexture::kClearcoat: return m.clearcoatTexture;
	case MaterialTexture::kClearcoatRoughness: return m.clearcoatRoughnessTexture;
	case MaterialTexture::kClearcoatNormal: return m.clearcoatNormalTexture;
	case MaterialTexture::kSheenColor: return m.sheenColorTexture;
	case MaterialTexture::kSheenRoughness: return m.sheenRoughnessTexture;
	case MaterialTexture::kTransmission: return m.transmissionTexture;
	case MaterialTexture::kThickness: return m.thicknessTexture;
	case MaterialTexture::kAnisotropy: return m.anisotropyTexture;
	case MaterialTexture::kIridescence: return m.iridescenceTexture;
	case MaterialTexture::kIridescenceThickness: return m.iridescenceThicknessTexture;
	case MaterialTexture::kDiffuseTransmission: return m.diffuseTransmissionTexture;
	case MaterialTexture::kDiffuseTransmissionColor: return m.diffuseTransmissionColorTexture;
	}
	return m.normalTexture;
}

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
		if (auto error = DecodeMeshopt(data))
			return std::unexpected(std::format("{}: {}", path.string(), *error));
	}

	if (auto result = cgltf_validate(&data); result != cgltf_result_success)
		return std::unexpected(std::format("{} is invalid: {}", path.string(), ToString(result)));

	if (isCancelled())
		return std::unexpected("cancelled");

	Mesh mesh;
	auto& stats = mesh.stats;
	auto warn = [&stats]<typename... Args>(std::format_string<Args...> fmt, Args&&... args)
	{ stats.warnings.push_back(std::format(fmt, std::forward<Args>(args)...)); };

	// KHR_animation_pointer: what each channel's pointer targets. the node transforms and weights are channels as any
	// other; the node visibilities, material values and texture transforms get a ScenePointerTarget each (a texture's
	// transform three: offset, rotation and scale, see TextureRef::animatedTransform), with their values at rest
	auto pointers = AnimationPointers(data);
	std::vector<std::vector<ParsedPointer>> parsedPointers(pointers.size());
	std::vector<uint8_t> visibilityMoves(data.nodes_count, 0);
	std::vector<uint8_t> animatedBaseColor(data.materials_count, 0);
	std::vector<std::string> unsupportedPointers;
	{
		core::UnorderedMap<uint64_t, int32_t> targets; // the first of each, by kind, index and property
		auto addTarget = [&mesh](ScenePointerTarget::Kind kind, uint32_t index, uint16_t property, std::span<const float> rest)
		{
			auto& animation = mesh.animation;
			animation.pointerTargets.push_back(ScenePointerTarget{
				.kind = kind,
				.index = index,
				.property = property,
				.valueCount = static_cast<uint16_t>(rest.size()),
				.valueBase = static_cast<uint32_t>(animation.pointerDefaults.size())});
			animation.pointerDefaults.insert(animation.pointerDefaults.end(), rest.begin(), rest.end());
			return static_cast<int32_t>(animation.pointerTargets.size() - 1);
		};
		for (size_t animationIt = 0; animationIt < pointers.size(); animationIt++)
		{
			parsedPointers[animationIt].resize(pointers[animationIt].size());
			for (size_t channelIt = 0; channelIt < pointers[animationIt].size(); channelIt++)
			{
				const auto& pointer = pointers[animationIt][channelIt];
				if (pointer.empty())
					continue;
				auto& parsedPointer = parsedPointers[animationIt][channelIt];
				parsedPointer = ParsePointer(pointer, data);
				using Kind = ParsedPointer::Kind;
				auto key = (static_cast<uint64_t>(std::to_underlying(parsedPointer.kind)) << 48U) |
						   (static_cast<uint64_t>(parsedPointer.index) << 16U) | parsedPointer.property;
				switch (parsedPointer.kind)
				{
				case Kind::kNone:
					if (!std::ranges::contains(unsupportedPointers, pointer))
						unsupportedPointers.push_back(pointer);
					break;
				case Kind::kNodeVisibility:
				{
					auto [it, inserted] = targets.try_emplace(key, -1);
					if (inserted)
					{
						std::array rest{IsHidden(data.nodes[parsedPointer.index]) ? 0.0F : 1.0F};
						it->second = addTarget(ScenePointerTarget::Kind::kNodeVisibility, parsedPointer.index, 0, rest);
					}
					parsedPointer.target = it->second;
					visibilityMoves[parsedPointer.index] = 1;
					break;
				}
				case Kind::kMaterial:
				{
					// the emissive factor and strength together (the shader's emissive is their product), keyed by the factor's
					auto property = MaterialProperty{parsedPointer.property};
					bool emissive = property == MaterialProperty::kEmissive || property == MaterialProperty::kEmissiveStrength;
					auto emissiveKey = (static_cast<uint64_t>(std::to_underlying(Kind::kMaterial)) << 48U) |
									   (static_cast<uint64_t>(parsedPointer.index) << 16U) | std::to_underlying(MaterialProperty::kEmissive);
					auto [it, inserted] = targets.try_emplace(emissive ? emissiveKey : key, -1);
					if (inserted)
					{
						const auto& material = data.materials[parsedPointer.index];
						auto first = emissive ? MaterialProperty::kEmissive : property;
						it->second = addTarget(
							ScenePointerTarget::Kind::kMaterial, parsedPointer.index, std::to_underlying(first), MaterialPropertyRest(material, first));
						if (emissive)
							addTarget(
								ScenePointerTarget::Kind::kMaterial, parsedPointer.index, std::to_underlying(MaterialProperty::kEmissiveStrength),
								MaterialPropertyRest(material, MaterialProperty::kEmissiveStrength));
					}
					parsedPointer.target = it->second + (property == MaterialProperty::kEmissiveStrength ? 1 : 0);
					if (MaterialProperty{parsedPointer.property} == MaterialProperty::kBaseColor)
						animatedBaseColor[parsedPointer.index] = 1;
					break;
				}
				case Kind::kTextureOffset:
				case Kind::kTextureRotation:
				case Kind::kTextureScale:
				{
					// all three parts, keyed by the offset's
					auto offsetKey = (static_cast<uint64_t>(std::to_underlying(Kind::kTextureOffset)) << 48U) |
									 (static_cast<uint64_t>(parsedPointer.index) << 16U) | parsedPointer.property;
					auto [it, inserted] = targets.try_emplace(offsetKey, -1);
					if (inserted)
					{
						const auto& view = MaterialTextureView(data.materials[parsedPointer.index], MaterialTexture{parsedPointer.property});
						std::array offset{0.0F, 0.0F};
						std::array rotation{0.0F};
						std::array scale{1.0F, 1.0F};
						if (view.has_transform)
						{
							offset = {view.transform.offset[0], view.transform.offset[1]};
							rotation = {view.transform.rotation};
							scale = {view.transform.scale[0], view.transform.scale[1]};
						}
						it->second = addTarget(ScenePointerTarget::Kind::kTextureOffset, parsedPointer.index, parsedPointer.property, offset);
						addTarget(ScenePointerTarget::Kind::kTextureRotation, parsedPointer.index, parsedPointer.property, rotation);
						addTarget(ScenePointerTarget::Kind::kTextureScale, parsedPointer.index, parsedPointer.property, scale);
					}
					parsedPointer.target = it->second + (parsedPointer.kind == Kind::kTextureOffset	  ? 0
														 : parsedPointer.kind == Kind::kTextureRotation ? 1
																										: 2);
					break;
				}
				case Kind::kLight:
				case Kind::kCamera:
				{
					auto [it, inserted] = targets.try_emplace(key, -1);
					if (inserted)
					{
						auto rest = parsedPointer.kind == Kind::kLight
										? LightPropertyRest(data.lights[parsedPointer.index], LightProperty{parsedPointer.property})
										: CameraPropertyRest(data.cameras[parsedPointer.index], CameraProperty{parsedPointer.property});
						it->second = addTarget(
							parsedPointer.kind == Kind::kLight ? ScenePointerTarget::Kind::kLight : ScenePointerTarget::Kind::kCamera,
							parsedPointer.index, parsedPointer.property, rest);
					}
					parsedPointer.target = it->second;
					break;
				}
				default: break; // a node's transform or weights
				}
			}
		}
	}

	// materials, and how their vertices take texcoords
	Images images(data, path, stats);
	std::vector<std::array<float, 4>> baseColorFactors(data.materials_count);
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
		// an animated base color factor is the material's (MaterialData::color), not baked into the vertex colors
		baseColorFactors[materialIt] = animatedBaseColor[materialIt] != 0 ? std::array{1.0F, 1.0F, 1.0F, 1.0F} : factor;
		std::copy_n(factor.begin(), 3, material.diffuse.begin());
		material.dissolve = factor[3];

		// each texture with its texcoord set, transform and sampler, which the shader applies (see TextureRef)
		auto textureRef = [&](const cgltf_texture_view& view)
		{
			TextureRef ref;
			auto resolved = images.Resolve(view, material.name);
			ref.path = resolved.path.string();
			ref.embeddedImage = resolved.embeddedImage;
			if (ref.empty())
				return ref;
			ref.texCoord = static_cast<uint32_t>(view.has_transform && view.transform.has_texcoord ? view.transform.texcoord : view.texcoord);
			if (ref.texCoord > 1)
			{
				warn("material {}: texcoord set {} isn't supported, set 0 is used", material.name, ref.texCoord);
				ref.texCoord = 0;
			}
			if (view.has_transform)
				ref.transform = TransformMatrix(view.transform);
			ref.sampler = SamplerOf(view.texture->sampler);
			return ref;
		};
		material.diffuseTexture = textureRef(*baseColorTexture);
		if (gltfMaterial.has_pbr_metallic_roughness)
		{
			const auto& pbr = gltfMaterial.pbr_metallic_roughness;
			material.metallic = pbr.metallic_factor;
			material.roughness = pbr.roughness_factor;
			material.metallicRoughnessTexture = textureRef(pbr.metallic_roughness_texture);
		}
		else if (gltfMaterial.has_pbr_specular_glossiness)
		{
			// a dielectric of its specular color and glossiness (see mesh::Material::specularGlossiness)
			const auto& specularGlossiness = gltfMaterial.pbr_specular_glossiness;
			material.specularGlossiness = true;
			material.metallic = 0.0F;
			material.glossiness = specularGlossiness.glossiness_factor;
			material.roughness = 1.0F - specularGlossiness.glossiness_factor;
			std::copy_n(specularGlossiness.specular_factor, 3, material.specularColor.begin());
			material.specularColorTexture = textureRef(specularGlossiness.specular_glossiness_texture);
		}
		else
		{
			// no pbrMetallicRoughness: its defaults, a rough metal
			material.metallic = 1.0F;
			material.roughness = 1.0F;
		}
		material.unlit = gltfMaterial.unlit != 0;
		if (gltfMaterial.has_specular && !material.specularGlossiness)
		{
			const auto& specular = gltfMaterial.specular;
			material.specular = std::clamp(specular.specular_factor, 0.0F, 1.0F);
			material.specularTexture = textureRef(specular.specular_texture);
			for (size_t channel = 0; channel < 3; channel++)
				material.specularColor[channel] = std::max(specular.specular_color_factor[channel], 0.0F);
			material.specularColorTexture = textureRef(specular.specular_color_texture);
		}
		if (gltfMaterial.has_clearcoat)
		{
			const auto& clearcoat = gltfMaterial.clearcoat;
			material.clearcoat = std::clamp(clearcoat.clearcoat_factor, 0.0F, 1.0F);
			material.clearcoatTexture = textureRef(clearcoat.clearcoat_texture);
			material.clearcoatRoughness = std::clamp(clearcoat.clearcoat_roughness_factor, 0.0F, 1.0F);
			material.clearcoatRoughnessTexture = textureRef(clearcoat.clearcoat_roughness_texture);
			material.clearcoatNormalTexture = textureRef(clearcoat.clearcoat_normal_texture);
			material.clearcoatNormalScale = clearcoat.clearcoat_normal_texture.scale;
		}
		if (gltfMaterial.has_sheen)
		{
			const auto& sheen = gltfMaterial.sheen;
			for (size_t channel = 0; channel < 3; channel++)
				material.sheenColor[channel] = std::max(sheen.sheen_color_factor[channel], 0.0F);
			material.sheenColorTexture = textureRef(sheen.sheen_color_texture);
			material.sheenRoughness = std::clamp(sheen.sheen_roughness_factor, 0.0F, 1.0F);
			material.sheenRoughnessTexture = textureRef(sheen.sheen_roughness_texture);
		}
		if (gltfMaterial.has_transmission)
		{
			material.transmission = std::clamp(gltfMaterial.transmission.transmission_factor, 0.0F, 1.0F);
			material.transmissionTexture = textureRef(gltfMaterial.transmission.transmission_texture);
		}
		if (gltfMaterial.has_volume)
		{
			const auto& volume = gltfMaterial.volume;
			material.thickness = std::max(volume.thickness_factor, 0.0F);
			material.thicknessTexture = textureRef(volume.thickness_texture);
			for (size_t channel = 0; channel < 3; channel++)
				material.attenuationColor[channel] = std::clamp(volume.attenuation_color[channel], 0.0F, 1.0F);
			// cgltf's default is FLT_MAX (none), as is the extension's (infinity)
			material.attenuationDistance =
				std::isfinite(volume.attenuation_distance) && volume.attenuation_distance < 1e30F ? std::max(volume.attenuation_distance, 0.0F) : 0.0F;
		}
		if (gltfMaterial.has_dispersion)
			material.dispersion = std::max(gltfMaterial.dispersion.dispersion, 0.0F);
		if (gltfMaterial.has_anisotropy)
		{
			const auto& anisotropy = gltfMaterial.anisotropy;
			material.anisotropy = std::clamp(anisotropy.anisotropy_strength, 0.0F, 1.0F);
			material.anisotropyRotation = anisotropy.anisotropy_rotation;
			material.anisotropyTexture = textureRef(anisotropy.anisotropy_texture);
		}
		if (gltfMaterial.has_iridescence)
		{
			const auto& iridescence = gltfMaterial.iridescence;
			material.iridescence = std::clamp(iridescence.iridescence_factor, 0.0F, 1.0F);
			material.iridescenceTexture = textureRef(iridescence.iridescence_texture);
			material.iridescenceIor = std::max(iridescence.iridescence_ior, 1.0F);
			material.iridescenceThicknessMin = std::max(iridescence.iridescence_thickness_min, 0.0F);
			material.iridescenceThicknessMax = std::max(iridescence.iridescence_thickness_max, 0.0F);
			material.iridescenceThicknessTexture = textureRef(iridescence.iridescence_thickness_texture);
		}
		if (gltfMaterial.has_diffuse_transmission)
		{
			const auto& diffuseTransmission = gltfMaterial.diffuse_transmission;
			material.diffuseTransmission = std::clamp(diffuseTransmission.diffuse_transmission_factor, 0.0F, 1.0F);
			material.diffuseTransmissionTexture = textureRef(diffuseTransmission.diffuse_transmission_texture);
			for (size_t channel = 0; channel < 3; channel++)
				material.diffuseTransmissionColor[channel] = std::max(diffuseTransmission.diffuse_transmission_color_factor[channel], 0.0F);
			material.diffuseTransmissionColorTexture = textureRef(diffuseTransmission.diffuse_transmission_color_texture);
		}
		// an ior of 0 is a perfect reflector's (the dielectric's f0 then 1), as the extension allows
		if (gltfMaterial.has_ior)
			material.ior = std::max(gltfMaterial.ior.ior, 0.0F);
		material.normalTexture = textureRef(gltfMaterial.normal_texture);
		material.normalScale = gltfMaterial.normal_texture.scale;
		auto emissiveStrength = gltfMaterial.has_emissive_strength ? gltfMaterial.emissive_strength.emissive_strength : 1.0F;
		for (size_t channel = 0; channel < 3; channel++)
			material.emissive[channel] = gltfMaterial.emissive_factor[channel] * emissiveStrength;
		material.emissiveTexture = textureRef(gltfMaterial.emissive_texture);
		material.occlusionTexture = textureRef(gltfMaterial.occlusion_texture);
		material.occlusionStrength = gltfMaterial.occlusion_texture.scale; // cgltf keeps the strength as scale

		switch (gltfMaterial.alpha_mode)
		{
		case cgltf_alpha_mode_opaque: material.alphaCutoff = 0.0F; break;
		case cgltf_alpha_mode_mask: material.alphaCutoff = gltfMaterial.alpha_cutoff; break;
		case cgltf_alpha_mode_blend:
			material.alphaCutoff = 0.0F;
			material.blend = true;
			break;
		default: break;
		}

		material.doubleSided = gltfMaterial.double_sided != 0;
	}
	// the textures whose transforms animations move, by their offset's target (see TextureRef::animatedTransform)
	for (size_t targetIt = 0; targetIt < mesh.animation.pointerTargets.size(); targetIt++)
		if (const auto& target = mesh.animation.pointerTargets[targetIt];
			target.kind == ScenePointerTarget::Kind::kTextureOffset && target.index < mesh.materials.size())
			MaterialTextureRef(mesh.materials[target.index], MaterialTexture{target.property}).animatedTransform = static_cast<int32_t>(targetIt);

	// primitives without a material get the spec's default one: white, metallic 1, roughness 1, opaque, single sided
	int32_t defaultMaterial = -1;
	for (cgltf_size meshIt = 0; meshIt < data.meshes_count && defaultMaterial < 0; meshIt++)
		for (cgltf_size primitiveIt = 0; primitiveIt < data.meshes[meshIt].primitives_count && defaultMaterial < 0; primitiveIt++)
			if (data.meshes[meshIt].primitives[primitiveIt].material == nullptr)
			{
				defaultMaterial = static_cast<int32_t>(mesh.materials.size());
				mesh.materials.push_back({.name = "default", .metallic = 1.0F, .roughness = 1.0F, .alphaCutoff = 0.0F});
				baseColorFactors.push_back({1.0F, 1.0F, 1.0F, 1.0F});
			}
	auto materialOf = [&data, defaultMaterial](const cgltf_primitive& primitive)
	{ return primitive.material != nullptr ? static_cast<int32_t>(primitive.material - data.materials) : defaultMaterial; };

	// the primitives of the scene's nodes, in their world space
	std::vector<Part> parts;
	size_t skippedPrimitives = 0;
	bool instancingWarned = false;
	bool morphWarned = false; // about weights that can't be applied
	double agreeingArea = 0.0;
	double normalArea = 0.0;

	// weights: the morph target weights (the node's, else the mesh's defaults), see the deltas below
	// instances: the part's instances (see Part), whose vertices are then in their node's space (world is the identity)
	auto addPrimitive = [&](const cgltf_primitive& primitive,
							const Matrix& world,
							std::span<const cgltf_float> weights,
							std::string_view meshName,
							const Part& instances) -> void
	{
		rhi::PrimitiveTopology topology{};
		switch (primitive.type)
		{
		case cgltf_primitive_type_triangles:
		case cgltf_primitive_type_triangle_strip:
		case cgltf_primitive_type_triangle_fan: topology = rhi::PrimitiveTopology::kTriangleList; break;
		case cgltf_primitive_type_lines:
		case cgltf_primitive_type_line_strip:
		case cgltf_primitive_type_line_loop: topology = rhi::PrimitiveTopology::kLineList; break;
		case cgltf_primitive_type_points: topology = rhi::PrimitiveTopology::kPointList; break;
		default:
			skippedPrimitives++;
			return;
		}

		const cgltf_accessor* positions = nullptr;
		const cgltf_accessor* normals = nullptr;
		const cgltf_accessor* tangents = nullptr;
		const cgltf_accessor* joints = nullptr;
		const cgltf_accessor* jointWeights = nullptr;
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
			case cgltf_attribute_type_tangent:
				if (attribute.index == 0)
					tangents = attribute.data;
				break;
			case cgltf_attribute_type_joints:
				if (attribute.index == 0)
					joints = attribute.data;
				break;
			case cgltf_attribute_type_weights:
				if (attribute.index == 0)
					jointWeights = attribute.data;
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
		// a draco compressed primitive's attributes and indices are decoded first, and read from there
		std::optional<DracoPrimitive> draco;
		if (primitive.has_draco_mesh_compression)
		{
			auto decoded = DecodeDraco(primitive, data);
			if (!decoded)
			{
				warn("mesh {}: a primitive is skipped: {}", meshName, decoded.error());
				skippedPrimitives++;
				return;
			}
			draco = std::move(*decoded);
		}

		auto unpack = [vertexCount, &draco](const cgltf_accessor* accessor, size_t components) -> std::optional<std::vector<float>>
		{
			if (accessor == nullptr || accessor->count != vertexCount)
				return std::nullopt;
			if (draco)
			{
				auto it = draco->values.find(accessor);
				if (it == draco->values.end() || it->second.size() != vertexCount * components)
					return std::nullopt;
				return it->second;
			}
			std::vector<float> values(vertexCount * components);
			if (cgltf_accessor_unpack_floats(accessor, values.data(), values.size()) != values.size())
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
		// tangents go with the file's normals: when normals are generated, gltf says to ignore them
		auto tangentValues = normalValues && tangents != nullptr && tangents->type == cgltf_type_vec4 ? unpack(tangents, 4) : std::nullopt;

		// morph targets: animated ones (instances.morphTargets) keep each target's position, normal and tangent (xyz)
		// deltas, for the vertex shader to weigh (see MorphDelta). others are applied here, at their weights
		bool animatedMorphs = instances.morphTargets > 0;
		std::vector<std::array<std::optional<std::vector<float>>, 3>> targetDeltas(animatedMorphs ? instances.morphTargets : 0);
		for (cgltf_size targetIt = 0; targetIt < primitive.targets_count; targetIt++)
		{
			auto weight = targetIt < weights.size() ? weights[targetIt] : 0.0F;
			if (weight == 0.0F && !animatedMorphs)
				continue;

			const auto& target = primitive.targets[targetIt];
			for (cgltf_size attributeIt = 0; attributeIt < target.attributes_count; attributeIt++)
			{
				const auto& attribute = target.attributes[attributeIt];
				auto* values = attribute.type == cgltf_attribute_type_position  ? &positionValues
							   : attribute.type == cgltf_attribute_type_normal  ? &normalValues
							   : attribute.type == cgltf_attribute_type_tangent ? &tangentValues
																				  : nullptr;
				if (values == nullptr || !*values || attribute.data->type != cgltf_type_vec3)
					continue;
				size_t stride = attribute.type == cgltf_attribute_type_tangent ? 4 : 3;

				auto deltas = unpack(attribute.data, 3);
				if (!deltas)
				{
					if (!std::exchange(morphWarned, true))
						warn("mesh {}: a morph target can't be read, and is ignored", meshName);
					continue;
				}
				if (animatedMorphs)
				{
					if (targetIt < targetDeltas.size())
						targetDeltas[targetIt][values == &positionValues ? 0 : values == &normalValues ? 1 : 2] = std::move(deltas);
					continue;
				}
				for (size_t i = 0; i < deltas->size(); i++)
					(**values)[((i / 3) * stride) + (i % 3)] += weight * (*deltas)[i];
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
		mesh.hasTangents |= tangentValues.has_value();
		mesh.hasTexCoords |= texCoordValues[0].has_value() || texCoordValues[1].has_value();
		mesh.hasColors |= colorValues.has_value();

		// the primitive's elements, in its vertices
		std::vector<uint32_t> elements;
		if (draco)
		{
			elements = std::move(draco->indices);
		}
		else if (primitive.indices != nullptr)
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
		std::vector<uint32_t> linesOrPoints; // a line list (pairs), or points
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
		case cgltf_primitive_type_lines:
			for (size_t i = 0; i + 1 < elements.size(); i += 2)
				linesOrPoints.insert(linesOrPoints.end(), {elements[i], elements[i + 1]});
			break;
		case cgltf_primitive_type_line_strip:
		case cgltf_primitive_type_line_loop:
			for (size_t i = 0; i + 1 < elements.size(); i++)
				linesOrPoints.insert(linesOrPoints.end(), {elements[i], elements[i + 1]});
			if (primitive.type == cgltf_primitive_type_line_loop && elements.size() > 2)
				linesOrPoints.insert(linesOrPoints.end(), {elements.back(), elements.front()});
			break;
		case cgltf_primitive_type_points: linesOrPoints = std::move(elements); break;
		default: break;
		}

		auto material = materialOf(primitive);
		auto factor = material >= 0 ? baseColorFactors[material] : std::array<float, 4>{1.0F, 1.0F, 1.0F, 1.0F};

		bool mirrored = Determinant(world) < 0.0;
		auto normalMatrix = NormalMatrix(world);
		if (mirrored)
			std::ranges::transform(normalMatrix, normalMatrix.begin(), [](double v) { return -v; });

		auto& part = parts.emplace_back(Part{
			.material = material,
			.topology = topology,
			.firstInstance = instances.firstInstance,
			.instanceCount = instances.instanceCount,
			.mirroredInstanceCount = instances.mirroredInstanceCount,
			.skin = instances.skin,
			.morphTargets = instances.morphTargets,
			.morphWeightBase = instances.morphWeightBase});

		// a skinned primitive's joints and weights, the weights normalized to sum to 1 (see SkinVertex)
		std::vector<SkinVertex> skinVertices;
		if (instances.skin >= 0)
		{
			skinVertices.resize(vertexCount);
			auto weightValues = jointWeights != nullptr && jointWeights->type == cgltf_type_vec4 ? unpack(jointWeights, 4) : std::nullopt;
			// decoded draco joints are floats, the others are read as they are
			auto jointValues = draco && joints != nullptr ? unpack(joints, 4) : std::nullopt;
			if (joints == nullptr || joints->type != cgltf_type_vec4 || joints->count != vertexCount || !weightValues || (draco && !jointValues))
			{
				warn("mesh {}: a skinned primitive without readable JOINTS_0 and WEIGHTS_0 is drawn in its bind pose", meshName);
				part.skin = -1;
				skinVertices.clear();
			}
			for (size_t vertexIt = 0; vertexIt < skinVertices.size(); vertexIt++)
			{
				std::array<cgltf_uint, 4> jointIndices{};
				if (jointValues)
					std::ranges::transform(std::span(*jointValues).subspan(4 * vertexIt, 4), jointIndices.begin(), [](float v) { return static_cast<cgltf_uint>(std::lround(v)); });
				else
					cgltf_accessor_read_uint(joints, vertexIt, jointIndices.data(), jointIndices.size());
				std::array<float, 4> weights{};
				std::copy_n(&(*weightValues)[4 * vertexIt], 4, weights.begin());
				auto sum = weights[0] + weights[1] + weights[2] + weights[3];
				auto& skinVertex = skinVertices[vertexIt];
				for (size_t i = 0; i < 4; i++)
				{
					auto weight = sum > 0.0F && std::isfinite(sum) ? std::clamp(weights[i] / sum, 0.0F, 1.0F) : (i == 0 ? 1.0F : 0.0F);
					auto unorm = static_cast<uint32_t>(std::lround(weight * 65535.0F));
					auto joint = std::min<cgltf_uint>(jointIndices[i], 0xffffU);
					skinVertex.joints[i / 2] |= joint << ((i % 2) * 16);
					skinVertex.weights[i / 2] |= unorm << ((i % 2) * 16);
				}
			}
		}

		// the vertices in world space, before normals are generated or repaired
		std::vector<VertexP3fN3fTa4fT014fC4f> vertices(vertexCount);
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

			// along +u, transformed like positions. a mirroring transform flips the handedness, as it does the
			// winding. unusable tangents are left at w = 0, which the shader replaces with a derived frame
			if (tangentValues)
			{
				const auto* t = &(*tangentValues)[4 * vertexIt];
				auto w = t[3];
				Vec3 tangent = TransformDirection(world, Vec3{t[0], t[1], t[2]});
				if (auto length = Length(tangent); IsFinite(tangent) && length > 0.0 && std::isfinite(w) && w != 0.0F)
				{
					for (size_t i = 0; i < 3; i++)
						vertex.tangent[i] = static_cast<float>(tangent[i] / length);
					vertex.tangent[3] = (w < 0.0F) != mirrored ? -1.0F : 1.0F;
				}
				else
				{
					stats.invalidTangents++;
				}
			}

			// both sets as they are: each texture reads its set, and transforms it (see TextureRef)
			for (size_t set = 0; set < texCoordValues.size(); set++)
			{
				if (!texCoordValues[set])
					continue;
				auto u = (*texCoordValues[set])[2 * vertexIt];
				auto v = (*texCoordValues[set])[(2 * vertexIt) + 1];
				if (!std::isfinite(u) || !std::isfinite(v))
				{
					stats.nonFiniteValues++;
					u = v = 0.0F;
				}
				vertex.texCoord01[2 * set] = u;
				vertex.texCoord01[(2 * set) + 1] = v;
			}

			std::array<float, 4> color{1.0F, 1.0F, 1.0F, 1.0F};
			if (colorValues)
				for (size_t i = 0; i < colorComponents; i++)
					color[i] = (*colorValues)[(colorComponents * vertexIt) + i];
			for (size_t i = 0; i < 4; i++)
				vertex.color[i] = color[i] * factor[i];
		}

		// the animated morph targets' deltas, a row per vertex, transformed as the vertex is: positions' and tangents' by
		// the node's transform, normals' by its cofactor matrix, and both scaled as their vertex's base vector was
		// normalized
		std::vector<MorphDelta> morphDeltas;
		if (animatedMorphs)
		{
			auto targets = static_cast<size_t>(instances.morphTargets);
			morphDeltas.resize(vertexCount * targets);
			auto at = [](const std::vector<float>& values, size_t vertexIt, size_t stride)
			{ return Vec3{values[stride * vertexIt], values[(stride * vertexIt) + 1], values[(stride * vertexIt) + 2]}; };
			auto store = [](float* out, const Vec3& v, double scale)
			{
				for (size_t axis = 0; axis < 3; axis++)
					out[axis] = static_cast<float>(v[axis] * scale);
			};
			for (size_t vertexIt = 0; vertexIt < vertexCount; vertexIt++)
			{
				double normalScale = 0.0;
				if (normalValues)
					if (auto length = Length(TransformNormal(normalMatrix, at(*normalValues, vertexIt, 3))); length > 0.0)
						normalScale = 1.0 / length;
				double tangentScale = 0.0;
				if (tangentValues)
					if (auto length = Length(TransformDirection(world, at(*tangentValues, vertexIt, 4))); length > 0.0)
						tangentScale = 1.0 / length;
				for (size_t targetIt = 0; targetIt < targets; targetIt++)
				{
					auto& delta = morphDeltas[(vertexIt * targets) + targetIt];
					const auto& [positions, normals, tangents] = targetDeltas[targetIt];
					if (positions)
						store(delta.position, TransformDirection(world, at(*positions, vertexIt, 3)), 1.0);
					if (normals)
						store(delta.normal, TransformNormal(normalMatrix, at(*normals, vertexIt, 3)), normalScale);
					if (tangents)
						store(delta.tangent, TransformDirection(world, at(*tangents, vertexIt, 3)), tangentScale);
				}
			}
		}
		// rows of a vertex's deltas, for the copies of it the parts make
		auto morphRow = [&morphDeltas, targets = static_cast<size_t>(instances.morphTargets)](size_t vertexIt)
		{ return std::span(morphDeltas).subspan(vertexIt * targets, targets); };

		// lines and points: as they are. without normals in the file they keep zero normals, and are drawn unlit
		if (topology != rhi::PrimitiveTopology::kTriangleList)
		{
			size_t size = topology == rhi::PrimitiveTopology::kLineList ? 2 : 1;
			for (size_t i = 0; i + size <= linesOrPoints.size(); i += size)
			{
				auto element = std::span(linesOrPoints).subspan(i, size);
				if (std::ranges::any_of(element, [vertexCount](uint32_t index) { return index >= vertexCount; }))
				{
					stats.droppedTriangles++;
					continue;
				}
				part.indices.insert(part.indices.end(), element.begin(), element.end());
				(size == 2 ? stats.lineCount : stats.pointCount)++;
			}
			part.vertices = std::move(vertices);
			part.skinVertices = std::move(skinVertices);
			part.morphDeltas = std::move(morphDeltas);
			return;
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
					if (!skinVertices.empty())
						part.skinVertices.push_back(skinVertices[corner]);
					if (animatedMorphs)
						std::ranges::copy(morphRow(corner), std::back_inserter(part.morphDeltas));
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
		{
			part.vertices = std::move(vertices);
			part.skinVertices = std::move(skinVertices);
			part.morphDeltas = std::move(morphDeltas);
		}

		// a normal map without tangents in the file: MikkTSpace's, as gltf says, from the normal map's texcoord set
		if (!tangentValues && material >= 0 && !mesh.materials[material].normalTexture.empty())
			stats.generatedTangents += GenerateTangents(part, mesh.materials[material].normalTexture.texCoord);
	};

	// the scene asked for, else the default scene (or the first), or all root nodes if there are no scenes
	for (cgltf_size sceneIt = 0; sceneIt < data.scenes_count; sceneIt++)
		mesh.scenes.push_back(data.scenes[sceneIt].name != nullptr ? data.scenes[sceneIt].name : std::format("scene {}", sceneIt));
	const cgltf_scene* scene = nullptr;
	if (options.scene && *options.scene < data.scenes_count)
		scene = &data.scenes[*options.scene];
	else if (options.scene && data.scenes_count > 0)
		warn("there is no scene {}, {} is loaded", *options.scene, data.scene != nullptr ? "the default one" : "the first");
	if (scene == nullptr)
		scene = data.scene != nullptr ? data.scene : data.scenes_count > 0 ? &data.scenes[0] : nullptr;
	std::vector<const cgltf_node*> roots;
	if (scene != nullptr)
	{
		mesh.scene = static_cast<uint32_t>(scene - data.scenes);
		for (cgltf_size nodeIt = 0; nodeIt < scene->nodes_count; nodeIt++)
			roots.push_back(scene->nodes[nodeIt]);
	}
	else
	{
		for (cgltf_size nodeIt = 0; nodeIt < data.nodes_count; nodeIt++)
			if (data.nodes[nodeIt].parent == nullptr)
				roots.push_back(&data.nodes[nodeIt]);
	}

	// the nodes animations move (their translation, rotation or scale), and their descendants: their meshes keep their
	// vertices in the node's space, drawn with instances that follow the node (see SceneAnimationData)
	std::vector<uint8_t> moves(data.nodes_count, 0);
	// the nodes whose morph target weights animations move: their meshes' targets are weighed on the gpu
	std::vector<uint8_t> morphsMove(data.nodes_count, 0);
	for (cgltf_size animationIt = 0; animationIt < data.animations_count; animationIt++)
	{
		const auto& animation = data.animations[animationIt];
		for (cgltf_size channelIt = 0; channelIt < animation.channels_count; channelIt++)
		{
			const auto& channel = animation.channels[channelIt];
			if (channel.target_node != nullptr &&
				(channel.target_path == cgltf_animation_path_type_translation ||
				 channel.target_path == cgltf_animation_path_type_rotation || channel.target_path == cgltf_animation_path_type_scale))
				moves[channel.target_node - data.nodes] = 1;
			if (channel.target_node != nullptr && channel.target_path == cgltf_animation_path_type_weights)
				morphsMove[channel.target_node - data.nodes] = 1;
			// KHR_animation_pointer's: a node whose visibility animates is kept in its space too, as is what is below it
			if (animationIt < parsedPointers.size() && channelIt < parsedPointers[animationIt].size())
			{
				const auto& parsedPointer = parsedPointers[animationIt][channelIt];
				switch (parsedPointer.kind)
				{
				case ParsedPointer::Kind::kNodeTranslation:
				case ParsedPointer::Kind::kNodeRotation:
				case ParsedPointer::Kind::kNodeScale:
				case ParsedPointer::Kind::kNodeVisibility: moves[parsedPointer.index] = 1; break;
				case ParsedPointer::Kind::kNodeWeights: morphsMove[parsedPointer.index] = 1; break;
				default: break;
				}
			}
		}
	}
	auto nodeMoves = [&data, &moves](const cgltf_node* node)
	{
		for (; node != nullptr; node = node->parent)
			if (moves[node - data.nodes] != 0)
				return true;
		return false;
	};

	// each skin a mesh uses, once: its joints (as node indices) and inverse bind matrices, and where its joint
	// matrices start
	core::UnorderedMap<size_t, int32_t> skinIndices;
	auto skinOf = [&](const cgltf_skin& gltfSkin) -> int32_t
	{
		auto key = static_cast<size_t>(&gltfSkin - data.skins);
		if (auto it = skinIndices.find(key); it != skinIndices.end())
			return it->second;

		SceneSkin skin;
		skin.jointBase = mesh.animation.jointCount;
		for (cgltf_size jointIt = 0; jointIt < gltfSkin.joints_count; jointIt++)
			skin.joints.push_back(static_cast<uint32_t>(gltfSkin.joints[jointIt] - data.nodes));
		skin.inverseBindMatrices.assign(skin.joints.size(), gfx::mesh::kIdentityTransform);
		if (const auto* accessor = gltfSkin.inverse_bind_matrices; accessor != nullptr && accessor->type == cgltf_type_mat4)
		{
			std::vector<float> values(accessor->count * 16);
			if (cgltf_accessor_unpack_floats(accessor, values.data(), values.size()) == values.size())
				for (size_t jointIt = 0; jointIt < std::min<size_t>(accessor->count, skin.joints.size()); jointIt++)
					std::copy_n(&values[jointIt * 16], 16, skin.inverseBindMatrices[jointIt].begin());
		}
		mesh.animation.jointCount += static_cast<uint32_t>(skin.joints.size());

		auto index = static_cast<int32_t>(mesh.animation.skins.size());
		mesh.animation.skins.push_back(std::move(skin));
		skinIndices.emplace(key, index);
		return index;
	};

	{
		ZoneScopedN("gltf::Import::nodes");

		std::vector<const cgltf_node*> stack(roots.rbegin(), roots.rend());
		while (!stack.empty())
		{
			const auto* node = stack.back();
			stack.pop_back();
			// hidden, unless an animation shows it (see NodeVisibility)
			if (IsHidden(*node) && visibilityMoves[node - data.nodes] == 0)
				continue;
			for (auto childIt = node->children_count; childIt > 0; childIt--)
				stack.push_back(node->children[childIt - 1]);

			if (node->camera != nullptr)
			{
				// animated where its node moves, or its values do
				auto& camera = mesh.cameras.emplace_back(SceneCameraOf(*node, data));
				camera.animated = nodeMoves(node) || std::ranges::any_of(mesh.animation.pointerTargets, [&camera](const ScenePointerTarget& target)
				{ return target.kind == ScenePointerTarget::Kind::kCamera && target.index == camera.source; });
			}
			if (node->light != nullptr)
			{
				// a light under a node that moves follows it (and its visibility)
				if (nodeMoves(node))
					mesh.animation.lightLinks.push_back(
						{.light = static_cast<uint32_t>(mesh.lights.size()), .node = static_cast<uint32_t>(node - data.nodes)});
				mesh.lights.push_back(SceneLightOf(*node, data));
			}

			if (node->mesh == nullptr)
				continue;

			auto nodeIndex = static_cast<uint32_t>(node - data.nodes);

			// a skinned mesh is placed by its joints, not its node: its vertices stay in the mesh's space
			Matrix world = gfx::mesh::kIdentityTransform;
			Part instances{};
			if (node->skin != nullptr)
				instances.skin = skinOf(*node->skin);
			else
				cgltf_node_transform_world(node, world.data());

			// a node's morph target weights override its mesh's defaults
			auto weights = node->weights_count > 0 ? std::span<const cgltf_float>(node->weights, node->weights_count)
												   : std::span<const cgltf_float>(node->mesh->weights, node->mesh->weights_count);

			std::string meshName = node->mesh->name != nullptr ? node->mesh->name : std::format("{}", node->mesh - data.meshes);

			// animated weights: the node's morph weights (its defaults, padded with zeros), and its primitives' deltas, which
			// the vertex shader weighs
			if (morphsMove[nodeIndex] != 0)
			{
				cgltf_size targetCount = 0;
				for (cgltf_size primitiveIt = 0; primitiveIt < node->mesh->primitives_count; primitiveIt++)
					targetCount = std::max(targetCount, node->mesh->primitives[primitiveIt].targets_count);
				if (targetCount > 0)
				{
					instances.morphTargets = static_cast<uint32_t>(targetCount);
					instances.morphWeightBase = static_cast<uint32_t>(mesh.morphWeights.size());
					mesh.animation.morphs.push_back(
						{.node = nodeIndex, .weightBase = instances.morphWeightBase, .weightCount = instances.morphTargets});
					for (cgltf_size targetIt = 0; targetIt < targetCount; targetIt++)
						mesh.morphWeights.push_back(targetIt < weights.size() ? weights[targetIt] : 0.0F);
				}
			}

			// instanced meshes (EXT_mesh_gpu_instancing) are drawn instanced: their vertices once, in the node's space, and
			// a transform per instance (the node's times the instance's), the mirroring ones last (see mesh::Submesh). so
			// is a mesh an animation moves, with one instance, linked to its node.
			auto transforms = node->has_mesh_gpu_instancing && instances.skin < 0 ? InstanceTransforms(node->mesh_gpu_instancing)
																				   : std::nullopt;
			if (node->has_mesh_gpu_instancing && !transforms && !std::exchange(instancingWarned, true))
				warn("mesh {}: its instances (EXT_mesh_gpu_instancing) can't be read, it is drawn once", meshName);
			bool moving = instances.skin < 0 && nodeMoves(node);
			if (moving || (transforms && !transforms->empty()))
			{
				// each instance's transform within the node, and at rest in world space
				auto locals = transforms && !transforms->empty() ? std::move(*transforms) : std::vector<Matrix>{gfx::mesh::kIdentityTransform};
				std::vector<size_t> order(locals.size());
				std::iota(order.begin(), order.end(), 0);
				auto mirrored = std::ranges::stable_partition(
					order, [&](size_t i) { return Determinant(Multiply(world, locals[i])) >= 0.0; });
				instances.firstInstance = static_cast<uint32_t>(mesh.instances.size());
				instances.instanceCount = static_cast<uint32_t>(order.size());
				instances.mirroredInstanceCount = static_cast<uint32_t>(mirrored.size());
				for (auto i : order)
				{
					if (moving)
						mesh.animation.instanceLinks.push_back(SceneInstanceLink{
							.instance = static_cast<uint32_t>(mesh.instances.size()), .node = nodeIndex, .local = locals[i]});
					mesh.instances.push_back(Multiply(world, locals[i]));
				}
				world = gfx::mesh::kIdentityTransform;
			}

			for (cgltf_size primitiveIt = 0; primitiveIt < node->mesh->primitives_count; primitiveIt++)
				addPrimitive(node->mesh->primitives[primitiveIt], world, weights, meshName, instances);

			if (isCancelled())
				return std::unexpected("cancelled");
		}
	}

	if (skippedPrimitives > 0)
		warn("{} primitives skipped (unreadable data)", skippedPrimitives);
	// what moves: all the nodes (by their gltf index), and the animations' translation, rotation and scale channels
	if (!mesh.animation.Empty())
	{
		for (cgltf_size nodeIt = 0; nodeIt < data.nodes_count; nodeIt++)
		{
			const auto& gltfNode = data.nodes[nodeIt];
			auto& node = mesh.animation.nodes.emplace_back();
			node.parent = gltfNode.parent != nullptr ? static_cast<int32_t>(gltfNode.parent - data.nodes) : -1;
			if (gltfNode.has_matrix)
			{
				node.hasMatrix = true;
				std::copy_n(gltfNode.matrix, 16, node.matrix.begin());
			}
			if (gltfNode.has_translation)
				std::copy_n(gltfNode.translation, 3, node.translation.begin());
			if (gltfNode.has_rotation)
				std::copy_n(gltfNode.rotation, 4, node.rotation.begin());
			if (gltfNode.has_scale)
				std::copy_n(gltfNode.scale, 3, node.scale.begin());
		}
	}
	size_t ignoredChannels = 0;
	for (cgltf_size animationIt = 0; animationIt < data.animations_count && !mesh.animation.Empty(); animationIt++)
	{
		const auto& gltfAnimation = data.animations[animationIt];
		auto& animation = mesh.animation.animations.emplace_back();
		animation.name = gltfAnimation.name != nullptr ? gltfAnimation.name : std::format("animation {}", animationIt);
		for (cgltf_size channelIt = 0; channelIt < gltfAnimation.channels_count; channelIt++)
		{
			const auto& gltfChannel = gltfAnimation.channels[channelIt];
			SceneAnimationChannel channel;
			size_t components = 3;
			// KHR_animation_pointer's channels have no target node or path in cgltf: the pointer's, or a ScenePointerTarget
			const cgltf_node* targetNode = gltfChannel.target_node;
			auto targetPath = gltfChannel.target_path;
			std::optional<int32_t> pointerTarget;
			if (animationIt < parsedPointers.size() && channelIt < parsedPointers[animationIt].size())
			{
				const auto& parsedPointer = parsedPointers[animationIt][channelIt];
				switch (parsedPointer.kind)
				{
				case ParsedPointer::Kind::kNodeTranslation: targetPath = cgltf_animation_path_type_translation; break;
				case ParsedPointer::Kind::kNodeRotation: targetPath = cgltf_animation_path_type_rotation; break;
				case ParsedPointer::Kind::kNodeScale: targetPath = cgltf_animation_path_type_scale; break;
				case ParsedPointer::Kind::kNodeWeights: targetPath = cgltf_animation_path_type_weights; break;
				case ParsedPointer::Kind::kNone: break;
				default: pointerTarget = parsedPointer.target; break;
				}
				if (parsedPointer.kind != ParsedPointer::Kind::kNone && !pointerTarget)
					targetNode = &data.nodes[parsedPointer.index];
			}
			if (pointerTarget && *pointerTarget >= 0)
			{
				channel.path = SceneAnimationChannel::Path::kPointer;
				components = mesh.animation.pointerTargets[*pointerTarget].valueCount;
				targetPath = cgltf_animation_path_type_invalid;
			}
			else switch (targetPath)
			{
			case cgltf_animation_path_type_translation: channel.path = SceneAnimationChannel::Path::kTranslation; break;
			case cgltf_animation_path_type_rotation:
				channel.path = SceneAnimationChannel::Path::kRotation;
				components = 4;
				break;
			case cgltf_animation_path_type_scale: channel.path = SceneAnimationChannel::Path::kScale; break;
			case cgltf_animation_path_type_weights:
			{
				// a node whose morphs are weighed on the gpu (see SceneMorph)
				channel.path = SceneAnimationChannel::Path::kWeights;
				auto morph = std::ranges::find(
					mesh.animation.morphs, targetNode != nullptr ? static_cast<uint32_t>(targetNode - data.nodes) : ~0U, &SceneMorph::node);
				components = morph != mesh.animation.morphs.end() ? morph->weightCount : 0;
				break;
			}
			default: components = 0; break;
			}
			const auto* sampler = gltfChannel.sampler;
			if (components == 0 || (targetNode == nullptr && !pointerTarget) || sampler == nullptr || sampler->input == nullptr ||
				sampler->output == nullptr)
			{
				ignoredChannels++;
				continue;
			}
			channel.node = pointerTarget ? static_cast<uint32_t>(*pointerTarget) : static_cast<uint32_t>(targetNode - data.nodes);
			channel.interpolation = sampler->interpolation == cgltf_interpolation_type_step ? SceneAnimationChannel::Interpolation::kStep
								  : sampler->interpolation == cgltf_interpolation_type_cubic_spline
									  ? SceneAnimationChannel::Interpolation::kCubicSpline
									  : SceneAnimationChannel::Interpolation::kLinear;
			channel.times.resize(sampler->input->count);
			// by the accessor's own type: weights are scalars, weightCount of them per key
			channel.values.resize(sampler->output->count * cgltf_num_components(sampler->output->type));
			auto valuesPerKey = components * (channel.interpolation == SceneAnimationChannel::Interpolation::kCubicSpline ? 3 : 1);
			if (cgltf_accessor_unpack_floats(sampler->input, channel.times.data(), channel.times.size()) != channel.times.size() ||
				cgltf_accessor_unpack_floats(sampler->output, channel.values.data(), channel.values.size()) != channel.values.size() ||
				channel.values.size() != channel.times.size() * valuesPerKey)
			{
				ignoredChannels++;
				continue;
			}
			if (!channel.times.empty())
				animation.duration = std::max(animation.duration, channel.times.back());
			animation.channels.push_back(std::move(channel));
		}
	}
	if (data.animations_count > 0 && mesh.animation.Empty())
		warn("{} animations are ignored (they move nothing drawn)", data.animations_count);
	if (ignoredChannels > 0)
		warn("{} animation channels are ignored (unreadable, or KHR_animation_pointer to what isn't drawn)", ignoredChannels);
	if (!unsupportedPointers.empty())
		warn(
			"{} KHR_animation_pointer targets aren't supported, e.g. {}", unsupportedPointers.size(),
			unsupportedPointers.front());
	if (normalArea > 0.0)
		stats.windingAgreement = agreeingArea / normalArea;

	// the parts in material order (-1 last), then by topology: one submesh per material and topology
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
		std::ranges::stable_sort(
			parts, {},
			[&bucketOf](const Part& part)
			{
				return std::tuple(
					bucketOf(part.material), part.topology, part.firstInstance, part.skin, part.morphTargets, part.morphWeightBase);
			});

		bool anySkinned = std::ranges::any_of(parts, [](const Part& part) { return part.skin >= 0; });
		// skinned vertices are bounded where their joints put them at rest, not in the mesh's (bind) space
		auto restJoints = anySkinned ? RestJoints(mesh.animation) : std::vector<SceneMatrix>{};
		bool firstVertex = true;
		for (auto& part : parts)
		{
			if (part.indices.empty())
				continue;

			// parts with the same morph weights share a submesh: their delta rows follow their vertices
			if (mesh.submeshes.empty() || mesh.submeshes.back().material != part.material ||
				mesh.submeshes.back().topology != part.topology || mesh.submeshes.back().firstInstance != part.firstInstance ||
				mesh.submeshes.back().skin != part.skin || mesh.submeshes.back().morphTargetCount != part.morphTargets ||
				mesh.submeshes.back().morphWeightBase != part.morphWeightBase)
				mesh.submeshes.push_back(Submesh{
					.firstIndex = static_cast<uint32_t>(mesh.indices.size()),
					.indexCount = 0,
					.material = part.material,
					.topology = part.topology,
					.firstInstance = part.firstInstance,
					.instanceCount = part.instanceCount,
					.mirroredInstanceCount = part.mirroredInstanceCount,
					.skin = part.skin,
					.morphTargetCount = part.morphTargets,
					.morphDeltaBase = static_cast<uint32_t>(mesh.morphDeltas.size()),
					.morphFirstVertex = static_cast<uint32_t>(mesh.vertices.size()),
					.morphWeightBase = part.morphWeightBase});
			if (part.morphTargets > 0)
				mesh.morphDeltas.insert(mesh.morphDeltas.end(), part.morphDeltas.begin(), part.morphDeltas.end());

			auto vertexOffset = static_cast<uint32_t>(mesh.vertices.size());
			for (auto index : part.indices)
				mesh.indices.push_back(vertexOffset + index);
			mesh.submeshes.back().indexCount += static_cast<uint32_t>(part.indices.size());

			auto addToBounds = [&mesh, &firstVertex](const std::array<float, 3>& position)
			{
				if (std::exchange(firstVertex, false))
				{
					mesh.bounds.SetMin(Bounds3f::VectorType(position[0], position[1], position[2]));
					mesh.bounds.SetMax(mesh.bounds.GetMin());
				}
				else
				{
					mesh.bounds.Merge(position);
				}
			};
			// where a vertex can go: at rest, and with animated morph targets, as far as their weights (0 to 1) take it
			// each way
			auto reach = [&part](size_t vertexIt, auto&& visit)
			{
				auto position = std::to_array(part.vertices[vertexIt].position);
				visit(position);
				if (part.morphTargets == 0)
					return;
				auto low = position;
				auto high = position;
				for (uint32_t targetIt = 0; targetIt < part.morphTargets; targetIt++)
				{
					const auto& delta = part.morphDeltas[(vertexIt * part.morphTargets) + targetIt].position;
					for (size_t axis = 0; axis < 3; axis++)
					{
						low[axis] += std::min(delta[axis], 0.0F);
						high[axis] += std::max(delta[axis], 0.0F);
					}
				}
				visit(low);
				visit(high);
			};
			if (part.skin >= 0 && part.skinVertices.size() == part.vertices.size())
			{
				auto jointBase = mesh.animation.skins[part.skin].jointBase;
				for (size_t vertexIt = 0; vertexIt < part.vertices.size(); vertexIt++)
					reach(vertexIt, [&](const auto& position) { addToBounds(SkinPosition(restJoints, jointBase, part.skinVertices[vertexIt], position)); });
			}
			else if (part.firstInstance == 0)
			{
				for (size_t vertexIt = 0; vertexIt < part.vertices.size(); vertexIt++)
					reach(vertexIt, addToBounds);
			}
			else if (!part.vertices.empty())
			{
				// the corners of its bounds at each instance
				std::array<double, 3> min{std::numeric_limits<double>::max(), std::numeric_limits<double>::max(), std::numeric_limits<double>::max()};
				std::array<double, 3> max{std::numeric_limits<double>::lowest(), std::numeric_limits<double>::lowest(), std::numeric_limits<double>::lowest()};
				for (size_t vertexIt = 0; vertexIt < part.vertices.size(); vertexIt++)
					reach(
						vertexIt,
						[&](const auto& position)
						{
							for (size_t axis = 0; axis < 3; axis++)
							{
								min[axis] = std::min<double>(min[axis], position[axis]);
								max[axis] = std::max<double>(max[axis], position[axis]);
							}
						});
				for (uint32_t instanceIt = part.firstInstance; instanceIt < part.firstInstance + part.instanceCount; instanceIt++)
					for (uint32_t corner = 0; corner < 8; corner++)
					{
						Vec3 p{(corner & 1U) != 0 ? max[0] : min[0], (corner & 2U) != 0 ? max[1] : min[1], (corner & 4U) != 0 ? max[2] : min[2]};
						auto q = TransformPoint(mesh.instances[instanceIt], p);
						addToBounds({static_cast<float>(q[0]), static_cast<float>(q[1]), static_cast<float>(q[2])});
					}
			}
			mesh.vertices.insert(mesh.vertices.end(), part.vertices.begin(), part.vertices.end());
			// skin vertices for all vertices if any are skinned (zero weights for the others)
			if (anySkinned)
			{
				if (part.skinVertices.size() == part.vertices.size())
					mesh.skinVertices.insert(mesh.skinVertices.end(), part.skinVertices.begin(), part.skinVertices.end());
				else
					mesh.skinVertices.resize(mesh.skinVertices.size() + part.vertices.size());
			}

			std::vector<VertexP3fN3fTa4fT014fC4f>().swap(part.vertices);
			std::vector<SkinVertex>().swap(part.skinVertices);
			std::vector<MorphDelta>().swap(part.morphDeltas);
			std::vector<uint32_t>().swap(part.indices);
		}
	}

	return mesh;
}

std::expected<std::vector<std::byte>, std::string> EmbeddedImage(const std::filesystem::path& path, uint32_t index)
{
	auto parsed = detail::Parse(path);
	if (!parsed)
		return std::unexpected(parsed.error());
	auto& data = **parsed;
	if (index >= data.images_count)
		return std::unexpected(std::format("{} has no image {}", path.string(), index));

	cgltf_options options{};
	if (auto result = cgltf_load_buffers(&options, &data, path.string().c_str()); result != cgltf_result_success)
		return std::unexpected(std::format("failed to load the buffers of {}: {}", path.string(), detail::ToString(result)));

	auto embedded = detail::ReadEmbedded(data.images[index]);
	if (!embedded)
		return std::unexpected(std::format("{}: image {}: {}", path.string(), index, embedded.error()));
	return std::move(embedded->bytes);
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
