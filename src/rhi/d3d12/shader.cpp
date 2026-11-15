// the D3D12 backend's shader modules: an entry point's DXIL (a container DXC compiled and signed, slang's DXIL target),
// which a pipeline state takes as its bytecode
#include <rhi/d3d12/backend.h>

#include <rhi/device.h>
#include <rhi/rhi.h>
#include <rhi/shader.h>

#include <core/assert.h>

#include <algorithm>
#include <string_view>
#include <utility>
#include <vector>

namespace rhi
{

namespace d3d12
{

const std::vector<char>& ShaderModuleObject::Code(uint32_t specialization) const
{
	if (specialization == 0)
		return code;
	auto found = std::ranges::find(specializations, specialization, &std::pair<uint32_t, std::vector<char>>::first);
	ENSUREF(
		found != specializations.end(),
		"{}: no DXIL of specialization {} (D3D12 has no specialization constants: the layout's configuration names the values compiled, SlangConfiguration::specializations)",
		name,
		specialization);
	return found->second;
}

} // namespace d3d12

IMPLEMENT_OBJECT_GETINSTANCE(ShaderModule<kD3D12>);
IMPLEMENT_DEVICEOBJECT_GETDEVICE(ShaderModule<kD3D12>);

template <>
ShaderModule<kD3D12>::ShaderModule(CreateDescType&& desc, ShaderModuleHandle<kD3D12>&& shaderModule, EntryPoint&& entryPoint)
	: SuperType(std::forward<CreateDescType>(desc))
	, myShaderModule(std::forward<ShaderModuleHandle<kD3D12>>(shaderModule))
	, myEntryPoint(std::forward<EntryPoint>(entryPoint))
{}

template <>
ShaderModule<kD3D12>::ShaderModule(CreateDescType&& desc)
	: ShaderModule(
		std::forward<CreateDescType>(desc),
		[&desc]
		{
			const auto& code = std::get<0>(desc.shader);
			ENSUREF(code.size() > 4 && std::string_view(code.data(), 4) == "DXBC", "{}: not DXIL (a DXBC container)", std::get<0>(std::get<1>(desc.shader)));
			return new d3d12::ShaderModuleObject{.name = std::get<0>(std::get<1>(desc.shader)), .code = code}; //NOLINT(cppcoreguidelines-owning-memory)
		}(),
		EntryPoint{std::get<1>(desc.shader)})
{}

template <>
ShaderModule<kD3D12>::~ShaderModule()
{
	delete myShaderModule; //NOLINT(cppcoreguidelines-owning-memory)
}

} // namespace rhi
