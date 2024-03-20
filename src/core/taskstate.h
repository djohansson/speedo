#pragma once

#include "copyableatomic.h"
#include "utils.h"

#include <optional>
#include <vector>

class Task;
using TaskHandle = Task*;
static constexpr TaskHandle NullTaskHandle = nullptr;

struct TaskState
{
	std::atomic_uint32_t latch{1u};
	std::vector<CopyableAtomic<TaskHandle>> adjacencies; // todo: thread safety
};
