#pragma once

#include "gfx-types.h"

template <class Archive>
void serialize(Archive& archive, ApplicationInfo<GraphicsBackend::Vulkan>& desc)
{
    archive(
        cereal::make_nvp("sType", desc.sType),
        //cereal::make_nvp("pApplicationName", desc.pApplicationName),
        cereal::make_nvp("applicationVersion", desc.applicationVersion),
        //cereal::make_nvp("pEngineName", desc.pEngineName),
        cereal::make_nvp("engineVersion", desc.engineVersion),
        cereal::make_nvp("apiVersion", desc.apiVersion));
}
