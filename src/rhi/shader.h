#pragma once

#include <rhi/deviceobject.h>
#include <rhi/shaderset.h>
#include <rhi/types.h>

namespace rhi
{

template <GraphicsApi G>
class ShaderModule;

template <GraphicsApi G>
struct ShaderModuleCreateDesc final : DeviceObjectCreateDesc<G>
{
	Shader shader;
};

template <GraphicsApi G>
struct ObjectTraits<ShaderModule<G>>
{
	using CreateDescType = ShaderModuleCreateDesc<G>;
};

template <GraphicsApi G>
class ShaderModule final : public DeviceObject<ShaderModule<G>>
{
public:
	using SuperType = DeviceObject<ShaderModule<G>>;
	using CreateDescType = ObjectTraits<ShaderModule<G>>::CreateDescType;

	constexpr ShaderModule() noexcept = default;
	explicit ShaderModule(CreateDescType&& desc);
	ShaderModule(ShaderModule&& other) noexcept;
	~ShaderModule();

	[[maybe_unused]] ShaderModule& operator=(ShaderModule&& other) noexcept;
	[[nodiscard]] operator auto() const noexcept { return myShaderModule; }//NOLINT(google-explicit-constructor)

	void Swap(ShaderModule& rhs) noexcept;
	friend void Swap(ShaderModule& lhs, ShaderModule& rhs) noexcept { lhs.Swap(rhs); }

	[[nodiscard]] const auto& GetEntryPoint() const noexcept { return myEntryPoint; }

private:
	ShaderModule( // takes ownership of provided handle
		CreateDescType&& desc,
		ShaderModuleHandle<G>&& shaderModule,
		const EntryPoint& entryPoint);

	ShaderModuleHandle<G> myShaderModule{};
	EntryPoint myEntryPoint{};
};

} // namespace rhi

