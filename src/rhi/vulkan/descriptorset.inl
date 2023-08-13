constexpr auto serialize(auto& archive, DescriptorSetLayoutBinding<Vk>& desc)
{
	return archive(
		desc.binding,
		desc.descriptorType,
		desc.descriptorCount,
		desc.stageFlags);
}

constexpr auto serialize(auto& archive, const DescriptorSetLayoutBinding<Vk>& desc)
{
	return archive(
		desc.binding,
		desc.descriptorType,
		desc.descriptorCount,
		desc.stageFlags);
}

constexpr auto serialize(auto& archive, SamplerCreateInfo<Vk>& desc)
{
	return archive(
		desc.flags,
		desc.magFilter,
		desc.minFilter,
		desc.mipmapMode,
		desc.addressModeU,
		desc.addressModeV,
		desc.addressModeW,
		desc.mipLodBias,
		desc.anisotropyEnable,
		desc.maxAnisotropy,
		desc.compareEnable,
		desc.compareOp,
		desc.minLod,
		desc.maxLod,
		desc.borderColor,
		desc.unnormalizedCoordinates);
}

constexpr auto serialize(auto& archive, const SamplerCreateInfo<Vk>& desc)
{
	return archive(
		desc.flags,
		desc.magFilter,
		desc.minFilter,
		desc.mipmapMode,
		desc.addressModeU,
		desc.addressModeV,
		desc.addressModeW,
		desc.mipLodBias,
		desc.anisotropyEnable,
		desc.maxAnisotropy,
		desc.compareEnable,
		desc.compareOp,
		desc.minLod,
		desc.maxLod,
		desc.borderColor,
		desc.unnormalizedCoordinates);
}
