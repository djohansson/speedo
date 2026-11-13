namespace rhi
{

// ShaderModule's Swap and moves: the same for every backend

template <GraphicsApi G>
void ShaderModule<G>::Swap(ShaderModule& rhs) noexcept
{
	SuperType::Swap(rhs);
	std::swap(myShaderModule, rhs.myShaderModule);
	std::swap(myEntryPoint, rhs.myEntryPoint);
}

template <GraphicsApi G>
ShaderModule<G>::ShaderModule(ShaderModule&& other) noexcept
{
	Swap(other);
}

template <GraphicsApi G>
ShaderModule<G>& ShaderModule<G>::operator=(ShaderModule&& other) noexcept
{
	Swap(other);
	return *this;
}

} // namespace rhi
