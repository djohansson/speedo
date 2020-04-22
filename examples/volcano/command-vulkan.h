#pragma once

#include <TracyVulkan.hpp>

namespace command_vulkan
{

struct UserData
{
#ifdef PROFILING_ENABLED
    TracyVkCtx tracyContext = nullptr;
#endif
};

}
