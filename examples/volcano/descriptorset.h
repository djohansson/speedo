#pragma once

#include "device.h"
#include "types.h"

#include <vector>

template <GraphicsBackend B>
struct SerializableDescriptorSetLayoutBinding : public DescriptorSetLayoutBinding<B>
{
	using BaseType = DescriptorSetLayoutBinding<B>;
};

template <GraphicsBackend B>
using DescriptorSetLayoutBindingsMap = std::map<uint32_t, std::vector<SerializableDescriptorSetLayoutBinding<B>>>; // set, bindings

template <GraphicsBackend B>
class DescriptorSetLayoutVector : public DeviceResource<B>
{
public:

    DescriptorSetLayoutVector(DescriptorSetLayoutVector&& other) = default;
    DescriptorSetLayoutVector(
        const std::shared_ptr<DeviceContext<B>>& deviceContext,
        const DescriptorSetLayoutBindingsMap<B>& bindings);
    ~DescriptorSetLayoutVector();

    auto size() const { return myDescriptorSetLayoutVector.size(); }
    auto data() const { return myDescriptorSetLayoutVector.data(); }

private:

    DescriptorSetLayoutVector( // uses provided vector
        const std::shared_ptr<DeviceContext<B>>& deviceContext,
        std::vector<DescriptorSetLayoutHandle<B>>&& descriptorSetLayoutVector);

	std::vector<DescriptorSetLayoutHandle<B>> myDescriptorSetLayoutVector;
};

template <GraphicsBackend B>
class DescriptorSetVector : public DeviceResource<B>
{
public:

    DescriptorSetVector(DescriptorSetVector&& other) = default;
    DescriptorSetVector(
        const std::shared_ptr<DeviceContext<B>>& deviceContext,
        const DescriptorSetLayoutVector<B>& layouts);
    DescriptorSetVector(
        const std::shared_ptr<DeviceContext<B>>& deviceContext,
        const DescriptorSetLayoutHandle<B>* layoutHandles,
        uint32_t layoutHandleCount);
    ~DescriptorSetVector();

    auto size() const { return myDescriptorSetVector.size(); }
    auto data() const { return myDescriptorSetVector.data(); }

private:

    DescriptorSetVector( // uses provided vector
        const std::shared_ptr<DeviceContext<B>>& deviceContext,
        std::vector<DescriptorSetHandle<B>>&& descriptorSetVector);

	std::vector<DescriptorSetHandle<B>> myDescriptorSetVector;
};
