// the Metal backend's shader modules: a library per entry point, compiled from slang's MSL by Metal's runtime compiler
// (MSL 3.2, fast math, as scripts/shadercheck.ps1 checks them; a whole file's MSL fails where entry points share
// groupshared memory, see docs/backends-plan.md)
#include <rhi/metal/backend.h>

#include <rhi/device.h>
#include <rhi/rhi.h>
#include <rhi/shader.h>

#include <core/assert.h>
#include <core/profiling.h>

#include <string>
#include <utility>

namespace rhi
{

IMPLEMENT_OBJECT_GETINSTANCE(ShaderModule<kMetal>);
IMPLEMENT_DEVICEOBJECT_GETDEVICE(ShaderModule<kMetal>);

template <>
ShaderModule<kMetal>::ShaderModule(CreateDescType&& desc, ShaderModuleHandle<kMetal>&& shaderModule, EntryPoint&& entryPoint)
	: SuperType(std::forward<CreateDescType>(desc))
	, myShaderModule(std::forward<ShaderModuleHandle<kMetal>>(shaderModule))
	, myEntryPoint(std::forward<EntryPoint>(entryPoint))
{}

template <>
ShaderModule<kMetal>::ShaderModule(CreateDescType&& desc)
	: ShaderModule(
		std::forward<CreateDescType>(desc),
		[&desc]
		{
			ZoneScopedN("ShaderModule::newLibraryWithSource");

			const auto& code = std::get<0>(desc.shader);
			const auto& entryPointName = std::get<0>(std::get<1>(desc.shader));
			const std::string text(code.data(), code.size()); // null terminated, as NS::String's copying init takes it
			auto* source = NS::String::alloc()->init(text.c_str(), NS::UTF8StringEncoding);
			auto* options = MTL::CompileOptions::alloc()->init();
			options->setLanguageVersion(MTL::LanguageVersion3_2);
			options->setMathMode(MTL::MathModeFast);
			NS::Error* error = nullptr;
			MTL::Library* library = desc.device->newLibrary(source, options, &error);
			options->release();
			source->release();
			ENSUREF(
				library != nullptr,
				"Failed to compile {}'s MSL: {}",
				entryPointName,
				error != nullptr ? error->localizedDescription()->utf8String() : "(no error)");
			library->setLabel(NS::String::string(entryPointName.c_str(), NS::UTF8StringEncoding));
			return library;
		}(),
		EntryPoint{std::get<1>(desc.shader)})
{}

template <>
ShaderModule<kMetal>::~ShaderModule()
{
	if (myShaderModule != nullptr)
		myShaderModule->release();
}

} // namespace rhi
