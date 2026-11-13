namespace rhi
{

// SamplerVector's Swap and moves: the same for every backend

template <GraphicsApi G>
void SamplerVector<G>::Swap(SamplerVector& rhs) noexcept
{
	DeviceObject<SamplerVector<G>>::Swap(rhs);
	std::swap(mySamplers, rhs.mySamplers);
}

template <GraphicsApi G>
SamplerVector<G>::SamplerVector(SamplerVector&& other) noexcept
{
	Swap(other);
}

template <GraphicsApi G>
SamplerVector<G>& SamplerVector<G>::operator=(SamplerVector&& other) noexcept
{
	Swap(other);
	return *this;
}

} // namespace rhi
