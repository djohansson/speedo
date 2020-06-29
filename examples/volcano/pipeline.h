#pragma once

#include "file.h"
#include "device.h"
#include "deviceresource.h"
#include "gfx.h" // remove

#include <memory>


template <GraphicsBackend B>
struct PipelineContextCreateDesc : DeviceResourceCreateDesc<B>
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
        PipelineContextCreateDesc<B>&& desc);
    ~PipelineContext();

    const auto& getConfig() const { return myConfig; }
    const auto getCache() const { return myCache; }

    // temp!
    void updateDescriptorSets(BufferHandle<GraphicsBackend::Vulkan> buffer) const;
    void createGraphicsPipeline();
    //

private:

    using PipelineMap = typename std::map<uint64_t, PipelineHandle<B>>;

    const PipelineContextCreateDesc<B> myDesc = {};
    PipelineCacheHandle<B> myCache = 0;
    PipelineMap myPipelineMap;
    std::optional<typename PipelineMap::const_iterator> myCurrent;


    // temp crap
    std::shared_ptr<InstanceContext<B>> myInstance;
    std::shared_ptr<PipelineConfiguration<B>> myConfig;
    std::filesystem::path myUserProfilePath;
    //
};
