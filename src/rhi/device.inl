#include <core/assert.h>
#include <core/uuids_extra.h>

namespace rhi
{

// the resources' set and the create descs: the same for every backend

template <GraphicsApi G>
typename Device<G>::ResourceSetType::const_iterator Device<G>::InternalGetResourceIterator(const uuids::uuid& uuid) const
{
	auto resourceIt = myResources.find(uuid);
	ENSUREF(resourceIt != myResources.end(), "no resource with uuid {}", uuids::to_string(uuid));
	return resourceIt;
}

template <GraphicsApi G>
bool Device<G>::HasResource(const uuids::uuid& uuid) const
{
	return myResources.contains(uuid);
}

template <GraphicsApi G>
void Device<G>::AddResource(std::shared_ptr<IObject> resource)
{
	ENSUREF(resource, "cannot add a null resource");
	ENSUREF(!resource->GetUuid().is_nil(), "resource must have a valid uuid");
	ENSUREF(myResources.insert(std::move(resource)).second, "resource is already stored");
}

template <GraphicsApi G>
std::shared_ptr<IObject> Device<G>::ReplaceResource(const uuids::uuid& previousUuid, std::shared_ptr<IObject> resource)
{
	std::shared_ptr<IObject> previous;
	if (!previousUuid.is_nil())
	{
		auto previousIt = InternalGetResourceIterator(previousUuid);
		previous = *previousIt;
		myResources.erase(previousIt);
	}
	if (resource)
		AddResource(std::move(resource));
	return previous;
}

template <GraphicsApi G>
std::shared_ptr<IObject> Device<G>::ReplaceResource(const std::shared_ptr<IObject>& previous, std::shared_ptr<IObject> resource)
{
	return ReplaceResource(previous ? previous->GetUuid() : uuids::uuid{}, std::move(resource));
}

template <GraphicsApi G>
void Device<G>::EraseResource(const uuids::uuid& uuid)
{
	if (uuid.is_nil())
		return;

	ENSUREF(myResources.erase(uuid) > 0, "no resource with uuid {}", uuids::to_string(uuid));
}

template <GraphicsApi G>
DeviceObjectCreateDesc<G> Device<G>::CreateDeviceObjectCreateDesc(std::string_view name) const noexcept
{
	return DeviceObjectCreateDesc<G>{
		ObjectCreateDesc<G>{
			.uuid = uuids::NewUuid(),
			.instance = this->GetDesc().instance,
			.name = std::string(name),
		},
		myDevice};
}

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
