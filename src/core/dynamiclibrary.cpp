#include <core/dynamiclibrary.h>

#include <format>

#ifdef _WIN32
#	ifndef WIN32_LEAN_AND_MEAN
#		define WIN32_LEAN_AND_MEAN
#	endif
#	ifndef NOMINMAX
#		define NOMINMAX
#	endif
#	include <windows.h>
#else
#	include <dlfcn.h>
#endif

namespace core
{

std::expected<DynamicLibrary, std::string> DynamicLibrary::Load(const std::filesystem::path& path) noexcept
{
#ifdef _WIN32
	HMODULE module = LoadLibraryW(path.wstring().c_str());
	if (module == nullptr)
		return std::unexpected(std::format("LoadLibraryW failed for {} (error {})", path.string(), GetLastError()));
	return DynamicLibrary(static_cast<void*>(module));
#else
	void* handle = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
	if (handle == nullptr)
	{
		const char* error = dlerror();
		return std::unexpected(error != nullptr ? std::string(error) : std::format("dlopen failed for {}", path.string()));
	}
	return DynamicLibrary(handle);
#endif
}

void* DynamicLibrary::Find(const char* name) const noexcept
{
#ifdef _WIN32
	return reinterpret_cast<void*>(GetProcAddress(static_cast<HMODULE>(myHandle), name));
#else
	return dlsym(myHandle, name);
#endif
}

} // namespace core
