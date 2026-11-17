#pragma once

#include <rhi/shaderset.h>

#include <zpp_bits.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// the shader compiler (the shadercompiler library: slang, and all the code that calls it): a shared library the client
// loads only when a shader must be compiled (see gfx::ShaderLoader: a checkout compiles them; an install's come
// precompiled), so a package ships neither it nor slang (31 MB of the macOS bundle's 79). a compile is a request and a
// response serialized with zpp::bits, plain bytes across the library boundary: each side has its own static libc++
namespace gfx::shadercompiler
{

// whether a compile's details are printed (SPEEDO_SHADER_LOG=1): its search paths, each parameter's reflection and each
// binding it makes, and each file it includes. thousands of lines a cold start, so off unless asked for
[[nodiscard]] inline bool Logging()
{
	static const bool kLog = []
	{
		const char* log = std::getenv("SPEEDO_SHADER_LOG");
		return log != nullptr && std::string_view(log) == "1";
	}();
	return kLog;
}

// whether a compile's times are printed (SPEEDO_SHADER_TIMINGS=1): each layout's rounds in the workers (the keys, the code
// jobs compiled and each worker's phases) or its phases in this process, and the scene's layouts each (see
// docs/shader-compile-plan.md)
[[nodiscard]] inline bool Timings()
{
	static const bool kTimings = []
	{
		const char* timings = std::getenv("SPEEDO_SHADER_TIMINGS");
		return timings != nullptr && std::string_view(timings) == "1";
	}();
	return kTimings;
}

// DXC's options for DXIL (slang hands them on): its sinking and global value numbering off, around a bug of WARP's
// (Microsoft's software adapter, 1.0.18 to 1.0.22 and 1.65535.22-preview, not Windows' own nor 1.0.17): its compiler
// removes the device at a dispatch of a shader with a resource's load in a loop with two exits, the load's result read
// after the loop (DXC's sinking moves the reads there), and of others GVN made (ComputeMainVisibility's: an unresolved
// handle). part of DXIL's cache key (ShaderLoader::ParametersHash). see docs/backends-plan.md, WARP: look again with
// each WARP release
inline constexpr std::array<const char*, 4> kDxcOptions{"-opt-disable", "sink", "-opt-disable", "gvn"};

struct EntryPoint
{
	std::string name;
	int32_t stage = 0; // SlangStage
};

// what a request asks for: the whole layout compiled in this process at once (kCompile: the shaders, their resources),
// or a share of it in a worker process (see gfx::ShaderLoader, docs/shader-compile-plan.md): each entry point's key and
// whether it reads the specialization constant (kKeys: Response::keys; with reflect, the resources too), or the code of
// some (kCode: Response::codes, one a job)
enum class Action : uint8_t
{
	kCompile,
	kKeys,
	kCode,
};

// a code job: an entry point's code (its index in Request::entryPoints), and, for a format without specialization
// constants (DXIL), with the constant a value (specialized: an entry point that reads it, its value 0 included)
struct CodeJob
{
	uint32_t entryPoint = 0;
	bool specialized = false;
	uint32_t value = 0;
};

// an entry point's key: a hash of its HLSL source as slang makes it from the linked program (everything it reaches, the
// definitions applied, its lines where there is debug information), which changes when what it compiles from does and
// not otherwise (slang's own entry point hash covers every file the module includes: one translation unit, so any edit
// changed all of them). whether that source reads the specialization constant, and its size (an estimate of the code's
// cost, where no compile of it has been timed)
struct EntryKey
{
	uint32_t entryPoint = 0;
	uint64_t low = 0;
	uint64_t high = 0;
	bool readsSpecialization = false;
	uint32_t sourceSize = 0;
};

// a slang file and how to compile it (see ShaderLoader::SlangConfiguration): slang's enums as their integers
struct Request
{
	Action action = Action::kCompile;
	rhi::ShaderFormat format{}; // the backend's (a copy of gfx's gfx::kShaderFormat), the binaries' format
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
	// the values the shaders' specialization constant 0 takes, compiled each for a format without specialization
	// constants (DXIL: rhi::ShaderSet::specializations); 0 always is
	std::vector<uint32_t> specializations;
	// kKeys: the entry points to key (indices into entryPoints), and whether to reflect the resources as well
	std::vector<uint32_t> keyEntryPoints;
	bool reflect = false;
	// kCode: the jobs
	std::vector<CodeJob> jobs;
};

// the compiled shaders and their resources, the files the source included (which the cache keeps, so that editing one
// recompiles), and slang's diagnostics (with the error, where it failed)
struct Response
{
	bool ok = false;
	std::string diagnostics;
	rhi::ShaderSet shaderSet; // kCompile's; kKeys' with reflect: the resources, the entry points without code
	std::vector<std::string> dependencies; // kCompile's, and kKeys' with reflect
	std::vector<EntryKey> keys; // kKeys': one a key entry point
	std::vector<rhi::ShaderBinary> codes; // kCode's: one a job
	std::vector<float> jobSeconds; // kCode's: each job's time
	std::string timings; // the phases' times (see Timings)
};

// the library's entry points (extern "C"): Compile takes a serialized Request and gives a serialized Response in memory
// of the library's, which the caller hands back to Free. one compile at a time (slang isn't thread safe: see ShaderLoader)
using CompileFn = int (*)(const std::byte* request, size_t requestSize, std::byte** response, size_t* responseSize);
using FreeFn = void (*)(std::byte* response);
inline constexpr const char* kCompileSymbol = "SpeedoShaderCompile";
inline constexpr const char* kFreeSymbol = "SpeedoShaderCompileFree";

// the worker process (tools/shadercompileworker.cpp, beside the client): `<worker> <request file> <response file>`, a
// serialized Request in, a serialized Response out; exit code 0 when the response is written (its ok says whether the
// compile did)

} // namespace gfx::shadercompiler
