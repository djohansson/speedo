// shadercompileworker <request file> <response file>: a shader compile in a process of its own (see gfx/shadercompiler.h):
// the serialized request handed to the shader compiler library beside it, the serialized response written. slang isn't
// thread safe, so gfx::ShaderLoader compiles a layout in several of these at once (docs/shader-compile-plan.md, step 2).
// exit code 0 once the response is written (its ok says whether the compile succeeded), otherwise the step that failed
#include <core/dynamiclibrary.h>
#include <core/file.h>
#include <core/path.h>
#include <gfx/shadercompiler.h>

#include <cstddef>
#include <filesystem>
#include <print>
#include <span>

int main(int argc, char** argv)
{
	if (argc != 3)
	{
		std::println(stderr, "usage: shadercompileworker <request file> <response file>");
		return 2;
	}
	auto executable = core::file::GetExecutablePath();
	if (!executable)
	{
		std::println(stderr, "shadercompileworker: the executable's path: {}", executable.error().message());
		return 3;
	}
	auto library = core::DynamicLibrary::Load(executable->parent_path() / SPEEDO_SHADERCOMPILER_FILE);
	if (!library)
	{
		std::println(stderr, "shadercompileworker: {}", library.error());
		return 4;
	}
	auto compile = reinterpret_cast<gfx::shadercompiler::CompileFn>(library->Find(gfx::shadercompiler::kCompileSymbol));
	auto free = reinterpret_cast<gfx::shadercompiler::FreeFn>(library->Find(gfx::shadercompiler::kFreeSymbol));
	if (compile == nullptr || free == nullptr)
	{
		std::println(stderr, "shadercompileworker: the shader compiler library has no {} or {}", gfx::shadercompiler::kCompileSymbol, gfx::shadercompiler::kFreeSymbol);
		return 5;
	}
	auto request = core::file::Map(argv[1]);
	if (!request)
	{
		std::println(stderr, "shadercompileworker: {}: {}", argv[1], request.error().message());
		return 6;
	}
	std::byte* response = nullptr;
	size_t responseSize = 0;
	if (int result = compile(request->data(), request->size(), &response, &responseSize); result != 0)
	{
		std::println(stderr, "shadercompileworker: the shader compiler failed on {} ({})", argv[1], result);
		return 7;
	}
	auto written = core::file::Write(argv[2], std::span<const std::byte>(response, responseSize));
	free(response);
	if (!written)
	{
		std::println(stderr, "shadercompileworker: {}: {}", argv[2], written.error().message());
		return 8;
	}
	return 0;
}
