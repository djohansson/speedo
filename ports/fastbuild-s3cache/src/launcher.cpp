// fbuild-s3cache: CMake's FASTBuild generator runs it as FBuild (CMAKE_MAKE_PROGRAM, the presets'), and it runs FBuild
// (vcpkg-tool-fastbuild's, in ../fastbuild from here) with the cache on and FBuildS3Cache (beside it) as the cache's plugin
// (see plugin.cpp, and docs/notes/build.md, "FASTBuild's cache").
//
// CMake can't name a cache plugin in the .bff it writes (no variable writes Settings' .CachePluginDLL), nor turn the cache
// on in `cmake --build`, so this does both: the plugin's two lines go into the .bff's Settings in place (whenever CMake has
// written it afresh; left as it is otherwise, so that FBuild doesn't parse it again), and -cache onto the command line
// (unless the command line or FASTBUILD_CACHE_MODE says otherwise). The remote's url and region come from the build
// directory's CMakeCache.txt (SPEEDO_FASTBUILD_CACHE_URL, SPEEDO_FASTBUILD_CACHE_REGION, which the presets set); never a
// credential, which the plugin reads from the environment. SPEEDO_FASTBUILD_CACHE=0 runs FBuild as it was asked. To go
// once a CMake release can name the plugin.
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#if defined(_WIN32)
#	ifndef WIN32_LEAN_AND_MEAN
#		define WIN32_LEAN_AND_MEAN
#	endif
#	ifndef NOMINMAX
#		define NOMINMAX
#	endif
#	include <windows.h>
#elif defined(__APPLE__)
#	include <mach-o/dyld.h>
#	include <unistd.h>
#else
#	include <unistd.h>
#endif

namespace
{

#if defined(_WIN32)
using Char = wchar_t;
#	define TEXT_LITERAL(text) L##text
constexpr std::string_view kFBuildName = "FBuild.exe";
constexpr std::string_view kPluginName = "FBuildS3Cache.dll";
#else
using Char = char;
#	define TEXT_LITERAL(text) text
#	if defined(__APPLE__)
constexpr std::string_view kFBuildName = "FBuild";
#	else
constexpr std::string_view kFBuildName = "fbuild"; // vcpkg-tool-fastbuild's name on Linux
#	endif
constexpr std::string_view kPluginName = "FBuildS3Cache.so"; // a CMake module's suffix on macOS too
#endif
using String = std::basic_string<Char>;

std::filesystem::path SelfPath()
{
#if defined(_WIN32)
	std::wstring buffer(32768, L'\0');
	DWORD size = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
	buffer.resize(size);
	return buffer;
#elif defined(__APPLE__)
	uint32_t size = 0;
	_NSGetExecutablePath(nullptr, &size);
	std::string buffer(size, '\0');
	_NSGetExecutablePath(buffer.data(), &size);
	return std::filesystem::canonical(buffer.c_str());
#else
	return std::filesystem::canonical("/proc/self/exe");
#endif
}

bool ReadBytes(const std::filesystem::path& path, std::string& out)
{
#if defined(_WIN32)
	std::FILE* file = _wfopen(path.c_str(), L"rb");
#else
	std::FILE* file = std::fopen(path.c_str(), "rb");
#endif
	if (file == nullptr)
		return false;
	out.clear();
	char buffer[65536];
	size_t count = 0;
	while ((count = std::fread(buffer, 1, sizeof(buffer), file)) > 0)
		out.append(buffer, count);
	std::fclose(file);
	return true;
}

bool WriteBytes(const std::filesystem::path& path, const std::string& bytes)
{
#if defined(_WIN32)
	std::FILE* file = _wfopen(path.c_str(), L"wb");
#else
	std::FILE* file = std::fopen(path.c_str(), "wb");
#endif
	if (file == nullptr)
		return false;
	bool ok = std::fwrite(bytes.data(), 1, bytes.size(), file) == bytes.size();
	return std::fclose(file) == 0 && ok;
}

// a cache variable's value in CMakeCache.txt (NAME:TYPE=value), empty where there is none
std::string CacheValue(const std::string& cache, std::string_view name)
{
	std::string prefix = std::string(name) + ":";
	for (size_t at = 0; at < cache.size();)
	{
		size_t end = cache.find('\n', at);
		std::string_view line(cache.data() + at, (end == std::string::npos ? cache.size() : end) - at);
		at = end == std::string::npos ? cache.size() : end + 1;
		if (line.starts_with(prefix))
		{
			size_t equals = line.find('=');
			if (equals == std::string_view::npos)
				return {};
			std::string_view value = line.substr(equals + 1);
			if (value.ends_with('\r'))
				value.remove_suffix(1);
			return std::string(value);
		}
	}
	return {};
}

// a value as a FASTBuild string's contents: ^ escapes ^, ' and $
std::string Escape(std::string_view text)
{
	std::string out;
	for (char c : text)
	{
		if (c == '^' || c == '\'' || c == '$')
			out += '^';
		out += c;
	}
	return out;
}

// the plugin's lines first in the .bff's Settings, replacing what this launcher wrote before; false where there are no
// Settings (not CMake's .bff)
bool AddPlugin(const std::filesystem::path& bff, const std::filesystem::path& plugin, const std::string& config)
{
	std::string text;
	if (!ReadBytes(bff, text))
		return false;
	std::string eol = text.find("\r\n") != std::string::npos ? "\r\n" : "\n";
	std::string opening = "Settings" + eol + "{" + eol;
	size_t at = text.find(opening);
	if (at == std::string::npos || (at != 0 && text[at - 1] != '\n'))
		return false;
	auto pluginPath = plugin.generic_u8string();
	std::string lines = "  .CachePluginDLL = '" + Escape(std::string(pluginPath.begin(), pluginPath.end())) + "'" + eol +
						"  .CachePluginDLLConfig = '" + Escape(config) + "'" + eol;
	size_t begin = at + opening.size();
	size_t end = begin;
	constexpr std::string_view kOurs = "  .CachePluginDLL";
	while (text.compare(end, kOurs.size(), kOurs) == 0)
	{
		size_t next = text.find('\n', end);
		end = next == std::string::npos ? text.size() : next + 1;
	}
	if (text.compare(begin, end - begin, lines) == 0)
		return true;
	text.replace(begin, end - begin, lines);
	return WriteBytes(bff, text);
}

bool Disabled()
{
	const char* value = std::getenv("SPEEDO_FASTBUILD_CACHE");
	return value != nullptr && std::string_view(value) == "0";
}

#if defined(_WIN32)
// an argument quoted as CommandLineToArgvW reads it back
std::wstring Quote(const std::wstring& argument)
{
	if (!argument.empty() && argument.find_first_of(L" \t\n\v\"") == std::wstring::npos)
		return argument;
	std::wstring out = L"\"";
	for (size_t at = 0;; ++at)
	{
		size_t backslashes = 0;
		while (at < argument.size() && argument[at] == L'\\')
		{
			++at;
			++backslashes;
		}
		if (at == argument.size())
		{
			out.append(backslashes * 2, L'\\');
			break;
		}
		if (argument[at] == L'"')
			out.append(backslashes * 2 + 1, L'\\');
		else
			out.append(backslashes, L'\\');
		out += argument[at];
	}
	return out + L"\"";
}

// FBuild run as a child, the launcher's directory first on its path (the plugin's dlls, which FBuild's LoadLibrary finds
// on the path and not beside the plugin); ctrl-c left to the child, which shares the console
int Run(const std::filesystem::path& fbuild, const std::vector<String>& arguments, const std::filesystem::path& directory)
{
	std::wstring path = directory.wstring();
	if (DWORD size = GetEnvironmentVariableW(L"PATH", nullptr, 0); size > 0)
	{
		std::wstring old(size, L'\0');
		old.resize(GetEnvironmentVariableW(L"PATH", old.data(), size));
		path += L";" + old;
	}
	SetEnvironmentVariableW(L"PATH", path.c_str());
	std::wstring commandLine = Quote(fbuild.wstring());
	for (const auto& argument : arguments)
		commandLine += L" " + Quote(argument);
	SetConsoleCtrlHandler(nullptr, TRUE);
	STARTUPINFOW startup{};
	startup.cb = sizeof(startup);
	PROCESS_INFORMATION process{};
	if (!CreateProcessW(fbuild.c_str(), commandLine.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr, &startup, &process))
	{
		std::fprintf(stderr, "fbuild-s3cache: can't run %ls (error %lu)\n", fbuild.c_str(), GetLastError());
		return 1;
	}
	WaitForSingleObject(process.hProcess, INFINITE);
	DWORD code = 1;
	GetExitCodeProcess(process.hProcess, &code);
	CloseHandle(process.hThread);
	CloseHandle(process.hProcess);
	return static_cast<int>(code);
}
#else
int Run(const std::filesystem::path& fbuild, const std::vector<String>& arguments, const std::filesystem::path& /*directory*/)
{
	std::vector<char*> argv;
	std::string program = fbuild.string();
	argv.push_back(program.data());
	std::vector<std::string> copies(arguments.begin(), arguments.end());
	for (auto& argument : copies)
		argv.push_back(argument.data());
	argv.push_back(nullptr);
	execv(program.c_str(), argv.data());
	std::perror(("fbuild-s3cache: can't run " + program).c_str());
	return 1;
}
#endif

} // namespace

#if defined(_WIN32)
int wmain(int argc, wchar_t** argv)
#else
int main(int argc, char** argv)
#endif
{
	std::filesystem::path directory = SelfPath().parent_path();
	std::filesystem::path fbuild = directory.parent_path() / "fastbuild" / kFBuildName;
	std::filesystem::path plugin = directory / kPluginName;
	std::vector<String> arguments(argv + 1, argv + argc);

	// CMake's build commands name the .bff (-config <build>/fbuild.bff); its other runs of FBuild (-version) don't
	std::filesystem::path bff;
	for (size_t at = 0; at + 1 < arguments.size(); ++at)
		if (arguments[at] == TEXT_LITERAL("-config"))
			bff = arguments[at + 1];
	if (!bff.empty() && !Disabled() && std::filesystem::exists(plugin))
	{
		std::string cache;
		ReadBytes(bff.parent_path() / "CMakeCache.txt", cache);
		std::string config = "url=" + CacheValue(cache, "SPEEDO_FASTBUILD_CACHE_URL") +
							 ";region=" + CacheValue(cache, "SPEEDO_FASTBUILD_CACHE_REGION");
		if (AddPlugin(bff, plugin, config))
		{
			// the cache's mode or an operation on it chosen already (not -cacheverbose nor -cachecompressionlevel)
			bool chosen = std::getenv("FASTBUILD_CACHE_MODE") != nullptr;
			for (const auto& argument : arguments)
				for (const Char* mode : {TEXT_LITERAL("-cache"), TEXT_LITERAL("-cacheread"), TEXT_LITERAL("-cachewrite"),
										 TEXT_LITERAL("-cacheinfo"), TEXT_LITERAL("-cachetrim")})
					chosen = chosen || argument == mode;
			if (!chosen)
				arguments.insert(arguments.begin(), TEXT_LITERAL("-cache"));
		}
		else
			std::fprintf(stderr, "fbuild-s3cache: no Settings in %s: building without the cache's plugin\n", bff.string().c_str());
	}
	return Run(fbuild, arguments, directory);
}
