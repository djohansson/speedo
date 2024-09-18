#pragma once

#include <atomic>
#include <string_view>

#include <rhi/capi.h>

template <GraphicsApi G>
struct RHI;
class TaskExecutor;

namespace scene
{

template <GraphicsApi G>
void LoadScene(RHI<G>& rhi, TaskExecutor& executor, std::string_view filePath, std::atomic_uint8_t& progress) {}

} // namespace scene
