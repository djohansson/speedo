#pragma once

#include <expected>
#include <filesystem>
#include <string>

namespace core
{

// a shared library loaded at run time by its path (dlopen, LoadLibraryW), kept loaded until the process ends (what it
// returns may be in use anywhere), and its exported symbols by name: for what the client loads only when it needs it (the
// shader compiler, see gfx::ShaderLoader). the error says why it couldn't be loaded
class DynamicLibrary final
{
public:
	[[nodiscard]] static std::expected<DynamicLibrary, std::string> Load(const std::filesystem::path& path) noexcept;

	// the exported symbol of that name, or null
	[[nodiscard]] void* Find(const char* name) const noexcept;

private:
	explicit DynamicLibrary(void* handle) noexcept : myHandle(handle) {}

	void* myHandle = nullptr;
};

} // namespace core
