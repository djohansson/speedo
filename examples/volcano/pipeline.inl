#include <cereal/cereal.hpp>
#include <cereal/archives/json.hpp>

template <class Archive, GraphicsBackend B>
void serialize(Archive& archive, PipelineConfiguration<B>& config)
{
    archive(cereal::make_nvp("cachePath", config.cachePath));
}
