namespace rhi
{

template <GraphicsApi G>
template <typename T>
std::shared_ptr<T> Device<G>::GetResource(const uuids::uuid& uuid) const
{
	return static_pointer_cast<T>(*InternalGetResourceIterator(uuid));
}

template <GraphicsApi G>
template <class T, class... Args>
std::shared_ptr<T> Device<G>::CreateResource(Args&&... args)
{
	auto resource = std::make_shared<T>(std::forward<Args>(args)...);
	AddResource(resource);
	return resource;
}

} // namespace rhi
