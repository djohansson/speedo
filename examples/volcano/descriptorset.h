#pragma once

#include "deviceresource.h"
#include "gfx-types.h"

#include <vector>

template <GraphicsBackend B>
class DescriptorSetLayout : public DeviceResource<B>
{
public:

    DescriptorSetLayout(DescriptorSetLayout&& other) noexcept = default;
    DescriptorSetLayout(
        const std::shared_ptr<DeviceContext<B>>& deviceContext,
        const std::vector<DescriptorSetLayoutBinding<B>>& bindings);
    ~DescriptorSetLayout();

    auto getDescrioptorSetLayoutHandle() const { return myDescriptorSetLayout; }

private:

    DescriptorSetLayout( // uses provided set handle
        const std::shared_ptr<DeviceContext<B>>& deviceContext,
        DescriptorSetLayoutHandle<B>&& descriptorSetLayout);

	DescriptorSetLayoutHandle<B> myDescriptorSetLayout = 0;
};



template <GraphicsBackend B>
class DescriptorSetVector : public DeviceResource<B>
{
public:

    DescriptorSetVector(DescriptorSetVector&& other) noexcept = default;
    DescriptorSetVector(
        const std::shared_ptr<DeviceContext<B>>& deviceContext,
        const DescriptorSetLayoutHandle<B>* layoutHandles,
        uint32_t layoutHandleCount);
    ~DescriptorSetVector();

    const auto& getDescriptorSetHandles() const { return myDescriptorSetVector; }

private:

    DescriptorSetVector( // uses provided vector
        const std::shared_ptr<DeviceContext<B>>& deviceContext,
        std::vector<DescriptorSetHandle<B>>&& descriptorSetVector);

	std::vector<DescriptorSetHandle<B>> myDescriptorSetVector;
};
