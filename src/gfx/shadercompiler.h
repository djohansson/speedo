#pragma once

#include <rhi/shaderset.h>

#include <zpp_bits.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

// the shader compiler (the shadercompiler library: slang, and all the code that calls it): a shared library the client
// loads only when a shader must be compiled (see gfx::ShaderLoader: a checkout compiles them; an install's come
// precompiled), so a package ships neither it nor slang (31 MB of the macOS bundle's 79). a compile is a request and a
// response serialized with zpp::bits, plain bytes across the library boundary: each side has its own static libc++
namespace gfx::shadercompiler
{

struct EntryPoint
{
	std::string name;
	int32_t stage = 0; // SlangStage
};

// a slang file and how to compile it (see ShaderLoader::SlangConfiguration): slang's enums as their integers
struct Request
{
	rhi::ShaderFormat format{}; // the build's (rhi::kShaderFormat), the binaries' format
	std::string file;
	std::vector<std::string> includePaths;
	std::string intermediatePath; // where slang dumps its intermediates, or none
	int32_t sourceLanguage = 0; // SlangSourceLanguage
	std::vector<EntryPoint> entryPoints;
	int32_t optimizationLevel = 0; // SlangOptimizationLevel
	int32_t debugInfoLevel = 0; // SlangDebugInfoLevel
	int32_t debugInfoFormat = 0; // SlangDebugInfoFormat
	int32_t matrixLayoutMode = 0; // SlangMatrixLayoutMode
	std::vector<std::pair<std::string, std::string>> definitions;
};

// the compiled shaders and their resources, the files the source included (which the cache keeps, so that editing one
// recompiles), and slang's diagnostics (with the error, where it failed)
struct Response
{
	bool ok = false;
	std::string diagnostics;
	rhi::ShaderSet shaderSet;
	std::vector<std::string> dependencies;
};

// the library's entry points (extern "C"): Compile takes a serialized Request and gives a serialized Response in memory
// of the library's, which the caller hands back to Free. one compile at a time (slang isn't thread safe: see ShaderLoader)
using CompileFn = int (*)(const std::byte* request, size_t requestSize, std::byte** response, size_t* responseSize);
using FreeFn = void (*)(std::byte* response);
inline constexpr const char* kCompileSymbol = "SpeedoShaderCompile";
inline constexpr const char* kFreeSymbol = "SpeedoShaderCompileFree";

} // namespace gfx::shadercompiler
