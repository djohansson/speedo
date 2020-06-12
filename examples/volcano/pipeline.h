#pragma once

#include "file.h"
#include "device.h"
#include "deviceresource.h"
#include "gfx.h" // remove

#include <memory>


template <GraphicsBackend B>
struct PipelineCreateDesc : DeviceResourceCreateDesc<B>
{
};

// todo: create maps with sensible hash keys for each structure that goes into vkCreateGraphicsPipelines()
//       combine them to get a compisite hash for the actual pipeline object

template <GraphicsBackend B>
class PipelineContext : public DeviceResource<B>
{
public:

    PipelineContext(PipelineContext&& other) = default;
    PipelineContext(
        const std::shared_ptr<InstanceContext<B>>& instanceContext,
        const std::shared_ptr<DeviceContext<B>>& deviceContext,
        const SerializableShaderReflectionModule<B>& shaders,
        const std::filesystem::path& userProfilePath,
        PipelineCreateDesc<B>&& desc);
    ~PipelineContext();

    const auto& getConfig() const { return myConfig; }
    const auto getCache() const { return myCache; }

    // temp!
    void updateDescriptorSets(BufferHandle<GraphicsBackend::Vulkan> buffer) const;
    void createGraphicsPipeline();
    //

private:

    const PipelineCreateDesc<B> myDesc = {};
	std::shared_ptr<PipelineConfiguration<B>> myConfig;
    PipelineCacheHandle<B> myCache = 0;


    // temp crap
    std::shared_ptr<InstanceContext<B>> myInstance;
    std::filesystem::path myUserProfilePath;
};
