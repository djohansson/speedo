#include <gfx/gltfimport.h>
#include <gfx/meshimport.h>
#include <gfx/objimport.h>

#include <algorithm>
#include <cctype>
#include <format>
#include <string>

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

} // namespace gfx::mesh
