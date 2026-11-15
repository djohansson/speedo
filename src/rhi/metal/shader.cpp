// the Metal backend's shader modules: a library per entry point, compiled from slang's MSL by Metal's runtime compiler
// (MSL 3.2, relaxed math, as scripts/shadercheck.ps1 checks them; a whole file's MSL fails where entry points share
// groupshared memory, see docs/backends-plan.md), asynchronously from the module's creation: the first pipeline made of
// it waits for it (metal::ShaderModuleObject)
#include <rhi/metal/backend.h>

#include <rhi/device.h>
#include <rhi/rhi.h>
#include <rhi/shader.h>

#include <core/assert.h>
#include <core/profiling.h>

#include <atomic>
#include <string>
#include <utility>

namespace rhi
{

namespace metal
{

MTL::Library* ShaderModuleObject::Library()
{
	while (!done.load(std::memory_order_acquire))
		done.wait(false, std::memory_order_acquire);
	ENSUREF(library != nullptr, "Failed to compile {}'s MSL: {}", name, error);
	return library;
}

} // namespace metal

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
			auto* object = new metal::ShaderModuleObject{.name = std::get<0>(std::get<1>(desc.shader))};
			const std::string text(code.data(), code.size()); // null terminated, as NS::String's copying init takes it
			auto* source = NS::String::alloc()->init(text.c_str(), NS::UTF8StringEncoding);
			auto* options = MTL::CompileOptions::alloc()->init();
			options->setLanguageVersion(MTL::LanguageVersion3_2);
			// infinities and NaNs kept, as the Vulkan drivers keep them (the user's call, 2026-11-15; KosmicKrisp's libraries
			// the same, ports/mesa's 0003 patch): relaxed math, which fast math was but for assuming there are none. the
			// fast functions and reassociation stay, within the precision Vulkan allows too: safe math and the precise
			// functions cost the M4 Pro a third of a frame each and bring no two gpus nearer (docs/notes/core.md)
			options->setMathMode(MTL::MathModeRelaxed);
			options->setMathFloatingPointFunctions(MTL::MathFloatingPointFunctionsFast);
			// on Metal's thread when it's done (the module waits for it before it's freed: see the destructor)
			desc.device->newLibrary(
				source,
				options,
				[object](MTL::Library* library, NS::Error* error)
				{
					if (library != nullptr)
					{
						object->library = library->retain();
						object->library->setLabel(NS::String::string(object->name.c_str(), NS::UTF8StringEncoding));
					}
					else
						object->error = error != nullptr ? error->localizedDescription()->utf8String() : "(no error)";
					object->done.store(true, std::memory_order_release);
					object->done.notify_all();
				});
			options->release();
			source->release();
			return object;
		}(),
		EntryPoint{std::get<1>(desc.shader)})
{}

template <>
ShaderModule<kMetal>::~ShaderModule()
{
	if (myShaderModule == nullptr)
		return;
	// the compile's handler writes into the module: done first
	while (!myShaderModule->done.load(std::memory_order_acquire))
		myShaderModule->done.wait(false, std::memory_order_acquire);
	if (myShaderModule->library != nullptr)
		myShaderModule->library->release();
	delete myShaderModule; //NOLINT(cppcoreguidelines-owning-memory)
}

} // namespace rhi
