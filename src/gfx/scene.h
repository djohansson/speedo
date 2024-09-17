#pragma once

#include <atomic>
#include <string_view>

struct IRhi;
class TaskExecutor;

namespace scene
{

void LoadScene(IRhi& rhi, TaskExecutor& executor, std::string_view filePath, std::atomic_uint8_t& progress);

} // namespace scene
