#include <rhi/shader.h>
#include <rhi/device.h>
#include <rhi/instance.h>
#include <rhi/rhi.h>
#include <rhi/vulkan/utils.h>

namespace rhi
{

IMPLEMENT_OBJECT_GETINSTANCE(ShaderModule<kVk>);
IMPLEMENT_DEVICEOBJECT_GETDEVICE(ShaderModule<kVk>);

template <>
void ShaderModule<kVk>::Swap(ShaderModule& rhs) noexcept
{
	SuperType::Swap(rhs);
	std::swap(myShaderModule, rhs.myShaderModule);
	std::swap(myEntryPoint, rhs.myEntryPoint);
}

template <>
ShaderModule<kVk>::ShaderModule(
	CreateDescType&& desc,
	ShaderModuleHandle<kVk>&& shaderModule,
	EntryPoint&& entryPoint)
	: SuperType(std::forward<CreateDescType>(desc))
	, myShaderModule(std::forward<ShaderModuleHandle<kVk>>(shaderModule))
	, myEntryPoint(std::forward<EntryPoint>(entryPoint))
{}

template <>
ShaderModule<kVk>::ShaderModule(CreateDescType&& desc)
	: ShaderModule<kVk>(
		std::forward<CreateDescType>(desc),
		[this, &desc](const auto& codePtr, size_t codeSize)
		{
			VkShaderModuleCreateInfo info{.sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
			info.codeSize = codeSize;
			info.pCode = codePtr;

			VkShaderModule vkShaderModule;
			VK_CHECK(vkCreateShaderModule(desc.device, &info, &GetInstance().GetHostAllocationCallbacks(), &vkShaderModule));
			Track(desc.device, VK_OBJECT_TYPE_SHADER_MODULE, vkShaderModule, GetDebugName(desc));
			return vkShaderModule;
		}(reinterpret_cast<const uint32_t*>(std::get<0>(desc.shader).data()), std::get<0>(desc.shader).size()),
		EntryPoint{std::get<1>(desc.shader)})
{}

template <>
ShaderModule<kVk>::ShaderModule(ShaderModule&& other) noexcept
{
	Swap(other);
}

template <>
ShaderModule<kVk>::~ShaderModule()
{
	if (myShaderModule == nullptr)
		return;

	Untrack(VK_OBJECT_TYPE_SHADER_MODULE, myShaderModule);
	vkDestroyShaderModule(
		GetDevice(),
		myShaderModule,
		&GetInstance().GetHostAllocationCallbacks());
}

template <>
ShaderModule<kVk>& ShaderModule<kVk>::operator=(ShaderModule&& other) noexcept
{
	Swap(other);
	return *this;
}

} // namespace rhi
