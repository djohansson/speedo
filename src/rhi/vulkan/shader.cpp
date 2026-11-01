#include <rhi/shader.h>
#include <rhi/device.h>
#include <rhi/instance.h>
#include <rhi/rhi.h>
#include <rhi/vulkan/utils.h>

#include <vector>

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

namespace shader
{

// a module's code as the device takes it: a fragment shader reading its primitive (SV_PrimitiveID, the visibility
// buffer's pre-pass) declares the Geometry capability (slang's choice), which needs the geometryShader feature, but
// Vulkan takes the Tessellation capability for it too, which KosmicKrisp has where it has no geometry shaders: there it
// is declared instead (or Shader again, where Tessellation is declared already)
std::vector<uint32_t> DeviceCode(const uint32_t* code, size_t wordCount, const VkPhysicalDeviceFeatures& features)
{
	constexpr uint32_t kHeaderWordCount = 5;
	constexpr uint32_t kOpCapability = 17;
	constexpr uint32_t kCapabilityShader = 1;
	constexpr uint32_t kCapabilityGeometry = 2;
	constexpr uint32_t kCapabilityTessellation = 3;
	std::vector<uint32_t> result(code, code + wordCount);
	if (features.geometryShader == VK_TRUE || features.tessellationShader != VK_TRUE)
		return result;
	auto capabilities = [&result](auto&& fn)
	{
		// the capabilities come first, right after the header
		for (size_t wordIt = kHeaderWordCount; wordIt < result.size();)
		{
			uint32_t count = result[wordIt] >> 16;
			if ((result[wordIt] & 0xffffU) != kOpCapability || count < 2)
				break;
			fn(result[wordIt + 1]);
			wordIt += count;
		}
	};
	bool tessellation = false;
	capabilities([&tessellation](uint32_t& capability) { tessellation = tessellation || capability == kCapabilityTessellation; });
	capabilities(
		[tessellation](uint32_t& capability)
		{
			if (capability == kCapabilityGeometry)
				capability = tessellation ? kCapabilityShader : kCapabilityTessellation;
		});
	return result;
}

} // namespace shader

template <>
ShaderModule<kVk>::ShaderModule(CreateDescType&& desc)
	: ShaderModule<kVk>(
		std::forward<CreateDescType>(desc),
		[this, &desc](const auto& codePtr, size_t codeSize)
		{
			// read from desc, not GetDesc(): this runs before the delegated constructor has initialized the base
			const auto& features = GetInstance().GetPhysicalDeviceInfo(GetDevice(desc.device).GetPhysicalDevice()).deviceFeatures.features;
			auto code = shader::DeviceCode(codePtr, codeSize / sizeof(uint32_t), features);
			VkShaderModuleCreateInfo info{.sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
			info.codeSize = code.size() * sizeof(uint32_t);
			info.pCode = code.data();

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
